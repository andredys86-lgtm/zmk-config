#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zmk/event_manager.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/keycode_state_changed.h>

#include "prog.h"
#include "keylist.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define MENU_LAYER 3
#define MATRIX_KEYS 16
#define EV_CONFIRM 16
#define EV_NEXT 17
#define EV_PREV 18

#define HOLD_MS 30
#define GAP_MS 30
#define MSG_MS 800

#define GRID_CELLS 12
#define CELL_MOD 7
#define CELL_DELAY 8
#define CELL_CLEAR 9
#define CELL_SAVE 10
#define CELL_CANCEL 11

#define MOD_CELLS 6
#define MOD_CLR_CELL 4
#define MOD_BACK_CELL 5

#define DLY_CELLS 12
#define DLY_NAV 11
#define DLY_CUSTOM_CELL 9
#define DLY_BACK_CELL 10

#define UNDO_MAX 64
#define MEDIA_PAGE 0x0Cu
#define MARKER "\xE2\x80\xA2"

enum { ST_PICK = 0, ST_GROUPS = 1, ST_KEYS = 2, ST_MODS = 3, ST_DELAY = 4 };
enum { U_ITEM = 0, U_MODON = 1, U_MARK = 2 };
enum { DLY_ONCE = 0, DLY_REPEAT = 1, DLY_OFF = 2 };

static const char *const grid_names[GRID_CELLS] = {
    "ABC", "NUM", "F-F", "NAV", "SYM", "KP", "MED", "MOD", "DELAY", "CLEAR", "SAVE", "CANCEL",
};

static const char *const mod_names[MOD_CELLS] = {
    "CTRL", "SHIFT", "ALT", "GUI", "M.CLR", "BACK",
};

static const char *const dly_names[DLY_NAV] = {
    "ONCE", "REPEAT", "OFF", "50", "100", "250", "500", "1000", "2000", "CUSTOM", "BACK",
};

static const uint16_t dly_values[6] = {50, 100, 250, 500, 1000, 2000};

static K_MUTEX_DEFINE(prog_lock);
static K_MUTEX_DEFINE(play_lock);

/* Stored sequences, one per programmable key */
static uint32_t seq[PROG_KEYS][PROG_SEQ_MAX];
static uint8_t seq_len[PROG_KEYS];

/* Menu state */
static uint8_t menu_state;
static int edit_target = -1;
static uint32_t edit_items[PROG_SEQ_MAX];
static uint8_t edit_len;
static int grp_cur;
static int cur_group;
static int key_page;
static int key_idx;
static int mod_cur;
static uint8_t mods; /* active modifiers: 1 = C, 2 = S, 4 = A, 8 = G */
static uint8_t msg_kind;
static int64_t msg_until;
static uint32_t version;

/* Delay settings of the current programming session */
static uint8_t dly_sel_mode; /* DLY_ONCE, DLY_REPEAT or DLY_OFF */
static uint16_t dly_ms;
static bool dly_armed;
static int dly_cur;

/* Edit history, in the order things were done. U_ITEM stands for the next
 * element of edit_items, U_MODON for a modifier that was switched on (bit in
 * the low nibble, 0x10 = all modifiers were off before), U_MARK for a
 * "modifiers cleared" marker (the previous mask in the low nibble). */
struct undo {
    uint8_t type;
    uint8_t val;
};
static struct undo ustack[UNDO_MAX];
static int ucount;

/* Playback state */
static struct {
    bool active;
    uint8_t len;
    uint8_t pos;
    uint8_t phase; /* 0 = start next element, 1 = release the held key */
    uint32_t held;
    uint32_t items[PROG_SEQ_MAX];
} pb;

static struct k_work_delayable play_work;

struct save_req {
    int idx;
    uint8_t len;
    uint32_t items[PROG_SEQ_MAX];
};

/* ---------- key groups ---------- */

static int group_count(int g) {
    int c = 0;
    for (size_t i = 0; i < key_list_len; i++) {
        if (key_list[i].group == g) {
            c++;
        }
    }
    return c;
}

static const struct key_entry *group_item(int g, int k) {
    int c = 0;
    for (size_t i = 0; i < key_list_len; i++) {
        if (key_list[i].group == g) {
            if (c == k) {
                return &key_list[i];
            }
            c++;
        }
    }
    return NULL;
}

static int per_page(int g) {
    return group_cols[g] * group_rows[g] - 1;
}

static int page_count(int g) {
    int n = group_count(g);
    int per = per_page(g);
    return (n + per - 1) / per;
}

static int cells_on_page(int g, int p) {
    int n = group_count(g);
    int per = per_page(g);
    int rem = n - p * per;
    if (rem > per) {
        rem = per;
    }
    if (rem < 0) {
        rem = 0;
    }
    return rem + 1; /* keys plus the BACK cell */
}

static void set_msg(uint8_t kind) {
    msg_kind = kind;
    msg_until = k_uptime_get() + MSG_MS;
}

/* ---------- editing helpers ---------- */

static bool is_media_code(uint32_t c) {
    return !PROG_IS_DELAY(c) && (((c >> 16) & 0xFFu) == MEDIA_PAGE);
}

static bool has_media(void) {
    for (int i = 0; i < edit_len; i++) {
        if (is_media_code(edit_items[i])) {
            return true;
        }
    }
    return false;
}

static bool has_normal(void) {
    for (int i = 0; i < edit_len; i++) {
        uint32_t c = edit_items[i];
        if (!PROG_IS_DELAY(c) && !is_media_code(c)) {
            return true;
        }
    }
    return false;
}

static void upush(uint8_t type, uint8_t val) {
    if (ucount < UNDO_MAX) {
        ustack[ucount].type = type;
        ustack[ucount].val = val;
        ucount++;
    }
}

static void reset_edit(void) {
    edit_len = 0;
    ucount = 0;
    mods = 0;
    dly_sel_mode = DLY_ONCE;
    dly_ms = 50;
    dly_armed = false;
    dly_cur = 0;
}

static void push_item(uint32_t code) {
    edit_items[edit_len++] = code;
    upush(U_ITEM, 0);
}

/* Adds one element together with the delays the current delay mode asks for.
 * Returns false (and shows FULL) when there is no room. */
static bool add_element(uint32_t code) {
    bool pre = false;
    bool post = false;

    if (dly_armed && dly_sel_mode == DLY_REPEAT && edit_len > 0 &&
        !PROG_IS_DELAY(edit_items[edit_len - 1])) {
        pre = true;
    }
    if (dly_armed && dly_sel_mode == DLY_ONCE) {
        post = true;
    }

    int need = 1 + (pre ? 1 : 0);
    if (edit_len + need > PROG_SEQ_MAX || ucount + need + 1 > UNDO_MAX) {
        set_msg(PROG_MSG_FULL);
        return false;
    }

    if (pre) {
        push_item(PROG_DELAY(dly_ms));
    }
    push_item(code);
    if (post) {
        if (edit_len < PROG_SEQ_MAX && ucount < UNDO_MAX) {
            push_item(PROG_DELAY(dly_ms));
        }
        /* a one-time delay switches itself off after use */
        dly_armed = false;
        dly_sel_mode = DLY_OFF;
    }
    return true;
}

/* true when the currently active modifiers were already applied to a key */
static bool mods_used(void) {
    int idx = (int)edit_len - 1;

    if (mods == 0) {
        return false;
    }
    for (int i = ucount - 1; i >= 0; i--) {
        const struct undo *u = &ustack[i];
        if (u->type == U_MARK) {
            return false;
        }
        if (u->type == U_MODON && (u->val & 0x10)) {
            return false;
        }
        if (u->type == U_ITEM) {
            if (idx >= 0) {
                uint32_t c = edit_items[idx];
                if (!PROG_IS_DELAY(c) && ((c >> 24) & 0x0F) != 0) {
                    return true;
                }
            }
            idx--;
        }
    }
    return false;
}

static void do_clear(void) {
    while (ucount > 0) {
        struct undo u = ustack[--ucount];
        if (u.type == U_ITEM) {
            if (edit_len > 0) {
                edit_len--;
            }
            return;
        }
        if (u.type == U_MARK) {
            mods = u.val & 0x0F;
            return;
        }
        uint8_t bit = u.val & 0x0F;
        if (mods & bit) {
            mods &= (uint8_t)~bit;
            return;
        }
        /* that modifier is already off: nothing to undo, look further back */
    }
}

static void mods_text(uint8_t m, char *out) {
    int p = 0;
    if (m & 1) {
        out[p++] = 'C';
    }
    if (m & 2) {
        out[p++] = 'S';
    }
    if (m & 4) {
        out[p++] = 'A';
    }
    if (m & 8) {
        out[p++] = 'G';
    }
    out[p] = '\0';
}

static uint8_t popcount4(uint8_t m) {
    return (uint8_t)((m & 1) + ((m >> 1) & 1) + ((m >> 2) & 1) + ((m >> 3) & 1));
}

static void item_text(uint32_t code, char *out, size_t n) {
    if (PROG_IS_DELAY(code)) {
        snprintf(out, n, "[%u]", (unsigned int)PROG_DELAY_MS(code));
        return;
    }

    uint8_t m = (code >> 24) & 0x0F;
    uint32_t base = code & 0x00FFFFFFu;
    const struct key_entry *best = NULL;
    uint8_t best_impl = 0;

    for (size_t i = 0; i < key_list_len; i++) {
        uint32_t ec = key_list[i].code;
        if ((ec & 0x00FFFFFFu) != base) {
            continue;
        }
        uint8_t impl = (ec >> 24) & 0x0F;
        if ((impl & m) != impl) {
            continue;
        }
        if (best == NULL || popcount4(impl) > popcount4(best_impl)) {
            best = &key_list[i];
            best_impl = impl;
        }
    }

    uint8_t expl = (uint8_t)(m & ~best_impl);
    char pre[8];
    mods_text(expl, pre);
    if (pre[0] != '\0') {
        snprintf(out, n, "%s:%s", pre, best ? best->name : "?");
    } else {
        snprintf(out, n, "%s", best ? best->name : "?");
    }
}

static void append_tok(char *out, size_t n, size_t *pos, const char *tok) {
    int w = snprintf(out + *pos, n - *pos, "%s%s", *pos ? " " : "", tok);
    if (w > 0 && (size_t)w < n - *pos) {
        *pos += (size_t)w;
    }
}

static void build_seq(char *out, size_t n) {
    size_t pos = 0;
    int idx = 0;
    char tmp[24];

    out[0] = '\0';
    for (int i = 0; i < ucount; i++) {
        if (ustack[i].type == U_ITEM) {
            if (idx < edit_len) {
                item_text(edit_items[idx], tmp, sizeof(tmp));
                idx++;
                append_tok(out, n, &pos, tmp);
            }
        } else if (ustack[i].type == U_MARK) {
            append_tok(out, n, &pos, MARKER);
        }
    }
    if (mods != 0) {
        char pre[8];
        mods_text(mods, pre);
        snprintf(tmp, sizeof(tmp), "%s:", pre);
        append_tok(out, n, &pos, tmp);
    }
}

/* ---------- storage ---------- */

static void save_seq(int idx, const uint32_t *items, uint8_t len) {
    char name[16];
    int rc;

    snprintf(name, sizeof(name), "np16/k%d", idx);
    if (len == 0) {
        rc = settings_delete(name);
    } else {
        rc = settings_save_one(name, items, (size_t)len * sizeof(uint32_t));
    }
    if (rc != 0) {
        LOG_ERR("np16 save failed: %d", rc);
    }
}

static int np16_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    const char *next;

    /* legacy single-array record from the old format: ignore it */
    if (settings_name_steq(name, "map", &next) && !next) {
        return 0;
    }

    if (name[0] == 'k') {
        char *end;
        long idx = strtol(name + 1, &end, 10);
        if (end == name + 1 || *end != '\0' || idx < 0 || idx >= PROG_KEYS) {
            return -ENOENT;
        }
        if (len == 0) {
            seq_len[idx] = 0;
            return 0;
        }
        if ((len % sizeof(uint32_t)) != 0 || (len / sizeof(uint32_t)) > PROG_SEQ_MAX) {
            return -EINVAL;
        }
        uint32_t tmp[PROG_SEQ_MAX];
        int rc = read_cb(cb_arg, tmp, len);
        if (rc < 0) {
            return rc;
        }
        memcpy(seq[idx], tmp, len);
        seq_len[idx] = (uint8_t)(len / sizeof(uint32_t));
        return 0;
    }
    return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(np16, "np16", NULL, np16_set, NULL, NULL);

/* ---------- playback ---------- */

static void play_step(struct k_work *work) {
    bool do_press = false;
    bool do_release = false;
    bool reschedule = false;
    uint32_t code = 0;
    uint32_t wait_ms = 0;

    k_mutex_lock(&play_lock, K_FOREVER);
    if (!pb.active) {
        k_mutex_unlock(&play_lock);
        return;
    }
    if (pb.phase == 1) {
        do_release = true;
        code = pb.held;
        pb.held = 0;
        pb.phase = 0;
        pb.pos++;
        if (pb.pos >= pb.len) {
            pb.active = false;
        } else {
            reschedule = true;
            wait_ms = GAP_MS;
        }
    } else {
        uint32_t e = pb.items[pb.pos];
        if (PROG_IS_DELAY(e)) {
            pb.pos++;
            if (pb.pos >= pb.len) {
                pb.active = false;
            } else {
                reschedule = true;
                wait_ms = PROG_DELAY_MS(e);
            }
        } else {
            do_press = true;
            code = e;
            pb.held = e;
            pb.phase = 1;
            reschedule = true;
            wait_ms = HOLD_MS;
        }
    }
    k_mutex_unlock(&play_lock);

    if (do_press) {
        raise_zmk_keycode_state_changed_from_encoded(code, true, k_uptime_get());
    }
    if (do_release) {
        raise_zmk_keycode_state_changed_from_encoded(code, false, k_uptime_get());
    }
    if (reschedule) {
        k_work_schedule(&play_work, K_MSEC(wait_ms));
    }
}

void prog_play(uint32_t idx) {
    uint32_t items[PROG_SEQ_MAX];
    uint8_t len;

    if (idx >= PROG_KEYS) {
        return;
    }

    k_mutex_lock(&prog_lock, K_FOREVER);
    len = seq_len[idx];
    if (len > 0) {
        memcpy(items, seq[idx], (size_t)len * sizeof(uint32_t));
    }
    k_mutex_unlock(&prog_lock);

    if (len == 0) {
        return;
    }

    k_mutex_lock(&play_lock, K_FOREVER);
    if (pb.active) {
        k_mutex_unlock(&play_lock);
        return;
    }
    pb.active = true;
    pb.len = len;
    pb.pos = 0;
    pb.phase = 0;
    pb.held = 0;
    memcpy(pb.items, items, (size_t)len * sizeof(uint32_t));
    k_mutex_unlock(&play_lock);

    k_work_schedule(&play_work, K_NO_WAIT);
}

static int prog_init(void) {
    k_work_init_delayable(&play_work, play_step);

    int rc = settings_subsys_init();
    if (rc != 0) {
        LOG_ERR("settings init failed: %d", rc);
    }
    settings_load_subtree("np16");
    return 0;
}

SYS_INIT(prog_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

static int prog_layer_listener(const zmk_event_t *eh) {
    const struct zmk_layer_state_changed *ev = as_zmk_layer_state_changed(eh);
    if (ev != NULL && ev->state && ev->layer == MENU_LAYER) {
        k_mutex_lock(&prog_lock, K_FOREVER);
        menu_state = ST_PICK;
        edit_target = -1;
        reset_edit();
        grp_cur = 0;
        cur_group = 0;
        key_page = 0;
        key_idx = 0;
        mod_cur = 0;
        msg_kind = PROG_MSG_NONE;
        version++;
        k_mutex_unlock(&prog_lock);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(prog_layer, prog_layer_listener);
ZMK_SUBSCRIPTION(prog_layer, zmk_layer_state_changed);

/* ---------- menu events ---------- */

static void pick_event(uint32_t ev) {
    if (ev < MATRIX_KEYS) {
        edit_target = (int)ev;
        reset_edit();
        edit_len = seq_len[ev];
        if (edit_len > 0) {
            memcpy(edit_items, seq[ev], (size_t)edit_len * sizeof(uint32_t));
        }
        for (int i = 0; i < edit_len; i++) {
            upush(U_ITEM, 0);
        }
        grp_cur = 0;
        msg_kind = PROG_MSG_NONE;
        menu_state = ST_GROUPS;
    }
}

static void groups_event(uint32_t ev, struct save_req *sr) {
    if (ev == EV_NEXT) {
        grp_cur = (grp_cur + 1) % GRID_CELLS;
    } else if (ev == EV_PREV) {
        grp_cur = (grp_cur + GRID_CELLS - 1) % GRID_CELLS;
    } else if (ev == EV_CONFIRM) {
        if (grp_cur < G_COUNT) {
            cur_group = grp_cur;
            key_page = 0;
            key_idx = 0;
            menu_state = ST_KEYS;
        } else if (grp_cur == CELL_MOD) {
            mod_cur = 0;
            menu_state = ST_MODS;
        } else if (grp_cur == CELL_DELAY) {
            dly_cur = 0;
            menu_state = ST_DELAY;
        } else if (grp_cur == CELL_CLEAR) {
            do_clear();
        } else if (grp_cur == CELL_SAVE) {
            /* an empty sequence is saved too: it erases the assignment */
            seq_len[edit_target] = edit_len;
            if (edit_len > 0) {
                memcpy(seq[edit_target], edit_items, (size_t)edit_len * sizeof(uint32_t));
                memcpy(sr->items, edit_items, (size_t)edit_len * sizeof(uint32_t));
            }
            sr->idx = edit_target;
            sr->len = edit_len;
            menu_state = ST_PICK;
            edit_target = -1;
            reset_edit();
            set_msg(PROG_MSG_SAVED);
        } else if (grp_cur == CELL_CANCEL) {
            menu_state = ST_PICK;
            edit_target = -1;
            reset_edit();
            set_msg(PROG_MSG_CANCELLED);
        }
    }
}

static void keys_event(uint32_t ev) {
    int g = cur_group;
    int pages = page_count(g);
    int cells = cells_on_page(g, key_page);

    if (ev == EV_NEXT) {
        if (key_idx < cells - 1) {
            key_idx++;
        } else if (key_page < pages - 1) {
            key_page++;
            key_idx = 0;
        }
    } else if (ev == EV_PREV) {
        if (key_idx > 0) {
            key_idx--;
        } else if (key_page > 0) {
            key_page--;
            key_idx = cells_on_page(g, key_page) - 1;
        } else {
            key_page = pages - 1;
            key_idx = cells_on_page(g, key_page) - 1;
        }
    } else if (ev == EV_CONFIRM) {
        if (key_idx == cells - 1) {
            menu_state = ST_GROUPS;
        } else {
            const struct key_entry *e = group_item(g, key_page * per_page(g) + key_idx);
            if (e != NULL) {
                bool media = (e->group == G_MED);
                /* a media command cannot be combined with other keys or modifiers */
                bool reject = media ? (mods != 0 || has_normal()) : has_media();
                if (!reject) {
                    uint32_t code = e->code;
                    if (!media) {
                        code |= ((uint32_t)mods << 24);
                    }
                    add_element(code);
                }
            }
            menu_state = ST_GROUPS;
        }
    }
}

static void mods_event(uint32_t ev) {
    if (ev == EV_NEXT) {
        mod_cur = (mod_cur + 1) % MOD_CELLS;
    } else if (ev == EV_PREV) {
        mod_cur = (mod_cur + MOD_CELLS - 1) % MOD_CELLS;
    } else if (ev == EV_CONFIRM) {
        if (mod_cur < 4) {
            uint8_t bit = (uint8_t)(1u << mod_cur);
            if (mods & bit) {
                mods &= (uint8_t)~bit;
                /* switched on and off again with nothing in between: no trace */
                if (ucount > 0 && ustack[ucount - 1].type == U_MODON &&
                    (ustack[ucount - 1].val & 0x0F) == bit) {
                    ucount--;
                }
            } else if (!has_media()) {
                uint8_t flag = (mods == 0) ? 0x10 : 0x00;
                mods |= bit;
                upush(U_MODON, (uint8_t)(bit | flag));
            }
            menu_state = ST_GROUPS;
        } else if (mod_cur == MOD_CLR_CELL) {
            if (mods != 0) {
                if (mods_used()) {
                    upush(U_MARK, mods);
                }
                mods = 0;
            }
            menu_state = ST_GROUPS;
        } else {
            menu_state = ST_GROUPS;
        }
    }
}

static void delay_event(uint32_t ev) {
    if (ev == EV_NEXT) {
        dly_cur = (dly_cur + 1) % DLY_NAV;
    } else if (ev == EV_PREV) {
        dly_cur = (dly_cur + DLY_NAV - 1) % DLY_NAV;
    } else if (ev == EV_CONFIRM) {
        if (dly_cur <= 2) {
            /* mode: ONCE, REPEAT or OFF */
            dly_sel_mode = (uint8_t)dly_cur;
            dly_armed = (dly_sel_mode != DLY_OFF);
        } else if (dly_cur < DLY_CUSTOM_CELL) {
            /* value: 50 ... 2000 ms */
            dly_ms = dly_values[dly_cur - 3];
            dly_armed = (dly_sel_mode != DLY_OFF);
        } else if (dly_cur == DLY_BACK_CELL) {
            menu_state = ST_GROUPS;
        }
        /* CUSTOM does nothing yet */
    }
}

void prog_menu_event(uint32_t ev) {
    struct save_req sr;
    sr.idx = -1;
    sr.len = 0;

    k_mutex_lock(&prog_lock, K_FOREVER);
    if (menu_state == ST_PICK) {
        pick_event(ev);
    } else if (menu_state == ST_GROUPS) {
        groups_event(ev, &sr);
    } else if (menu_state == ST_KEYS) {
        keys_event(ev);
    } else if (menu_state == ST_MODS) {
        mods_event(ev);
    } else {
        delay_event(ev);
    }
    version++;
    k_mutex_unlock(&prog_lock);

    if (sr.idx >= 0) {
        save_seq(sr.idx, sr.items, sr.len);
    }
}

void prog_get_menu_view(struct prog_menu_view *v) {
    k_mutex_lock(&prog_lock, K_FOREVER);
    memset(v, 0, sizeof(*v));
    v->mode = menu_state;
    v->version = version;
    v->msg_kind = (msg_kind != PROG_MSG_NONE && k_uptime_get() < msg_until) ? msg_kind
                                                                          : PROG_MSG_NONE;
    v->cols = 3;
    v->rows = 4;
    v->font = 10;

    if (menu_state == ST_GROUPS) {
        v->cells = GRID_CELLS;
        v->cursor = (uint8_t)grp_cur;
        for (int i = 0; i < GRID_CELLS; i++) {
            snprintf(v->cell[i], sizeof(v->cell[i]), "%s", grid_names[i]);
        }
        v->hl_mask = (mods != 0) ? (1u << CELL_MOD) : 0u;
        build_seq(v->seq, sizeof(v->seq));
    } else if (menu_state == ST_KEYS) {
        int g = cur_group;
        int per = per_page(g);
        int cells = cells_on_page(g, key_page);
        v->cols = group_cols[g];
        v->rows = group_rows[g];
        v->font = group_font[g];
        v->cells = (uint8_t)cells;
        v->cursor = (uint8_t)key_idx;
        for (int i = 0; i < cells - 1; i++) {
            const struct key_entry *e = group_item(g, key_page * per + i);
            snprintf(v->cell[i], sizeof(v->cell[i]), "%s", e ? e->name : "?");
        }
        snprintf(v->cell[cells - 1], sizeof(v->cell[cells - 1]), "%s",
                 group_font[g] == 10 ? "BACK" : "<-");
        build_seq(v->seq, sizeof(v->seq));
    } else if (menu_state == ST_MODS) {
        v->cols = 3;
        v->rows = 2;
        v->font = 10;
        v->cells = MOD_CELLS;
        v->cursor = (uint8_t)mod_cur;
        for (int i = 0; i < MOD_CELLS; i++) {
            snprintf(v->cell[i], sizeof(v->cell[i]), "%s", mod_names[i]);
        }
        v->hl_mask = (uint32_t)(mods & 0x0F);
        build_seq(v->seq, sizeof(v->seq));
    } else if (menu_state == ST_DELAY) {
        uint32_t hl = (1u << dly_sel_mode);
        v->cells = DLY_CELLS;
        v->cursor = (uint8_t)dly_cur;
        for (int i = 0; i < DLY_NAV; i++) {
            snprintf(v->cell[i], sizeof(v->cell[i]), "%s", dly_names[i]);
        }
        snprintf(v->cell[DLY_NAV], sizeof(v->cell[DLY_NAV]), "[%u]", (unsigned int)dly_ms);
        for (int i = 0; i < 6; i++) {
            if (dly_values[i] == dly_ms) {
                hl |= (1u << (3 + i));
            }
        }
        v->hl_mask = hl;
        build_seq(v->seq, sizeof(v->seq));
    }
    k_mutex_unlock(&prog_lock);
}

uint32_t prog_version(void) {
    return version;
}

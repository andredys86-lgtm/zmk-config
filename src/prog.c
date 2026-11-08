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
#define MSG_MS 1500

#define GRID_CELLS 12
#define CELL_CLEAR 9
#define CELL_SAVE 10
#define CELL_CANCEL 11
#define KEY_ROWS 4

enum { ST_PICK = 0, ST_GROUPS = 1, ST_KEYS = 2 };

static const char *const grid_names[GRID_CELLS] = {
    "ABC", "NUM", "F-F", "NAV", "SYM", "KP", "MED", "MOD", "DELAY", "CLEAR", "SAVE", "CANCEL",
};

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
static uint8_t msg_kind;
static int64_t msg_until;
static uint32_t version;

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
    return group_cols[g] * KEY_ROWS - 1;
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
        if (len == 0 || (len % sizeof(uint32_t)) != 0 ||
            (len / sizeof(uint32_t)) > PROG_SEQ_MAX) {
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
        edit_len = 0;
        grp_cur = 0;
        cur_group = 0;
        key_page = 0;
        key_idx = 0;
        msg_kind = PROG_MSG_NONE;
        version++;
        k_mutex_unlock(&prog_lock);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(prog_layer, prog_layer_listener);
ZMK_SUBSCRIPTION(prog_layer, zmk_layer_state_changed);

static void pick_event(uint32_t ev) {
    if (ev < MATRIX_KEYS) {
        edit_target = (int)ev;
        edit_len = seq_len[ev];
        if (edit_len > 0) {
            memcpy(edit_items, seq[ev], (size_t)edit_len * sizeof(uint32_t));
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
        } else if (grp_cur == CELL_CLEAR) {
            if (edit_len > 0) {
                edit_len--;
            }
        } else if (grp_cur == CELL_SAVE) {
            if (edit_len > 0) {
                seq_len[edit_target] = edit_len;
                memcpy(seq[edit_target], edit_items, (size_t)edit_len * sizeof(uint32_t));
                sr->idx = edit_target;
                sr->len = edit_len;
                memcpy(sr->items, edit_items, (size_t)edit_len * sizeof(uint32_t));
                menu_state = ST_PICK;
                edit_target = -1;
                edit_len = 0;
                set_msg(PROG_MSG_SAVED);
            }
        } else if (grp_cur == CELL_CANCEL) {
            menu_state = ST_PICK;
            edit_target = -1;
            edit_len = 0;
            set_msg(PROG_MSG_CANCELLED);
        }
        /* MED, MOD and DELAY do nothing yet */
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
                if (edit_len >= PROG_SEQ_MAX) {
                    set_msg(PROG_MSG_FULL);
                } else {
                    edit_items[edit_len++] = e->code;
                }
            }
            menu_state = ST_GROUPS;
        }
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
    } else {
        keys_event(ev);
    }
    version++;
    k_mutex_unlock(&prog_lock);

    if (sr.idx >= 0) {
        save_seq(sr.idx, sr.items, sr.len);
    }
}

static void build_seq(char *out, size_t n) {
    size_t pos = 0;
    out[0] = '\0';
    for (int i = 0; i < edit_len; i++) {
        char tmp[12];
        uint32_t e = edit_items[i];
        if (PROG_IS_DELAY(e)) {
            snprintf(tmp, sizeof(tmp), "[%u]", (unsigned int)PROG_DELAY_MS(e));
        } else {
            const char *l = key_label(e);
            snprintf(tmp, sizeof(tmp), "%s", l ? l : "?");
        }
        int w = snprintf(out + pos, n - pos, "%s%s", i ? " " : "", tmp);
        if (w < 0 || (size_t)w >= n - pos) {
            break;
        }
        pos += (size_t)w;
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

    if (menu_state == ST_PICK) {
        snprintf(v->hint, sizeof(v->hint), "hold knob: exit");
    } else if (menu_state == ST_GROUPS) {
        snprintf(v->hint, sizeof(v->hint), "Turn:go Push:OK");
        v->cells = GRID_CELLS;
        v->cursor = (uint8_t)grp_cur;
        for (int i = 0; i < GRID_CELLS; i++) {
            snprintf(v->cell[i], sizeof(v->cell[i]), "%s", grid_names[i]);
        }
        build_seq(v->seq, sizeof(v->seq));
    } else {
        int g = cur_group;
        int per = per_page(g);
        int cells = cells_on_page(g, key_page);
        snprintf(v->hint, sizeof(v->hint), "Turn:go Push:add");
        v->cols = group_cols[g];
        v->cells = (uint8_t)cells;
        v->cursor = (uint8_t)key_idx;
        for (int i = 0; i < cells - 1; i++) {
            const struct key_entry *e = group_item(g, key_page * per + i);
            snprintf(v->cell[i], sizeof(v->cell[i]), "%s", e ? e->name : "?");
        }
        snprintf(v->cell[cells - 1], sizeof(v->cell[cells - 1]), "%s",
                 group_cols[g] >= 4 ? "BACK" : "<-");
        build_seq(v->seq, sizeof(v->seq));
    }
    k_mutex_unlock(&prog_lock);
}

uint32_t prog_version(void) {
    return version;
}

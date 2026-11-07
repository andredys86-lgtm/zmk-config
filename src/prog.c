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

#define GROUP_ITEMS (G_COUNT + 2)

static K_MUTEX_DEFINE(prog_lock);
static K_MUTEX_DEFINE(play_lock);

/* Stored sequences, one per programmable key */
static uint32_t seq[PROG_KEYS][PROG_SEQ_MAX];
static uint8_t seq_len[PROG_KEYS];

/* 0 = wait for key, 1 = pick group, 2 = pick key inside group */
static uint8_t menu_state;
static int menu_target = -1;
static int menu_group;
static int menu_pos;
static bool menu_saved;
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

static int group_pos_of_code(int g, uint32_t code) {
    int c = 0;
    for (size_t i = 0; i < key_list_len; i++) {
        if (key_list[i].group == g) {
            if (key_list[i].code == code) {
                return c;
            }
            c++;
        }
    }
    return -1;
}

static uint32_t first_code(int idx) {
    if (idx < 0 || idx >= PROG_KEYS || seq_len[idx] == 0) {
        return 0;
    }
    return seq[idx][0];
}

static int list_len(void) {
    if (menu_state == 1) {
        return GROUP_ITEMS;
    }
    return 1 + group_count(menu_group);
}

static void item_name(int idx, char *out, size_t n) {
    if (menu_state == 1) {
        if (idx == 0) {
            snprintf(out, n, "< Cancel");
        } else if (idx == 1) {
            snprintf(out, n, "Clear key");
        } else {
            snprintf(out, n, "%s", group_names[idx - 2]);
        }
    } else {
        if (idx == 0) {
            snprintf(out, n, "< Back");
        } else {
            const struct key_entry *e = group_item(menu_group, idx - 1);
            snprintf(out, n, "%s", e ? e->name : "?");
        }
    }
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
        menu_state = 0;
        menu_target = -1;
        menu_group = 0;
        menu_pos = 0;
        menu_saved = false;
        version++;
        k_mutex_unlock(&prog_lock);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(prog_layer, prog_layer_listener);
ZMK_SUBSCRIPTION(prog_layer, zmk_layer_state_changed);

void prog_menu_event(uint32_t ev) {
    int save_idx = -1;
    uint8_t save_len = 0;
    uint32_t save_items[PROG_SEQ_MAX];

    k_mutex_lock(&prog_lock, K_FOREVER);
    if (menu_state == 0) {
        if (ev < MATRIX_KEYS) {
            menu_target = (int)ev;
            menu_state = 1;
            menu_pos = 2;
            menu_saved = false;
        }
    } else {
        int len = list_len();
        if (ev == EV_NEXT) {
            menu_pos = (menu_pos + 1) % len;
        } else if (ev == EV_PREV) {
            menu_pos = (menu_pos + len - 1) % len;
        } else if (ev == EV_CONFIRM) {
            if (menu_state == 1) {
                if (menu_pos == 0) {
                    menu_state = 0;
                    menu_target = -1;
                } else if (menu_pos == 1) {
                    seq_len[menu_target] = 0;
                    save_idx = menu_target;
                    save_len = 0;
                    menu_state = 0;
                    menu_target = -1;
                    menu_saved = true;
                } else {
                    menu_group = menu_pos - 2;
                    menu_state = 2;
                    int k = group_pos_of_code(menu_group, first_code(menu_target));
                    menu_pos = (k >= 0) ? k + 1 : 1;
                }
            } else {
                if (menu_pos == 0) {
                    menu_state = 1;
                    menu_pos = menu_group + 2;
                } else {
                    const struct key_entry *e = group_item(menu_group, menu_pos - 1);
                    if (e != NULL) {
                        seq[menu_target][0] = e->code;
                        seq_len[menu_target] = 1;
                        save_idx = menu_target;
                        save_len = 1;
                        save_items[0] = e->code;
                        menu_state = 0;
                        menu_target = -1;
                        menu_saved = true;
                    }
                }
            }
        }
    }
    version++;
    k_mutex_unlock(&prog_lock);

    if (save_idx >= 0) {
        save_seq(save_idx, save_items, save_len);
    }
}

void prog_get_view(struct prog_view *v) {
    char nm[24];

    k_mutex_lock(&prog_lock, K_FOREVER);
    v->title[0] = '\0';
    v->rows[0][0] = '\0';
    v->rows[1][0] = '\0';
    v->rows[2][0] = '\0';

    if (menu_state == 0) {
        snprintf(v->title, sizeof(v->title), "MENU");
        snprintf(v->rows[1], sizeof(v->rows[1]), "Press key");
        snprintf(v->rows[2], sizeof(v->rows[2]), "%s", menu_saved ? "Saved!" : "hold: exit");
    } else {
        int len = list_len();
        if (menu_state == 1) {
            snprintf(v->title, sizeof(v->title), "Key %d", menu_target + 1);
        } else {
            snprintf(v->title, sizeof(v->title), "Key %d: %s", menu_target + 1,
                     group_names[menu_group]);
        }
        item_name((menu_pos + len - 1) % len, nm, sizeof(nm));
        snprintf(v->rows[0], sizeof(v->rows[0]), "  %s", nm);
        item_name(menu_pos, nm, sizeof(nm));
        snprintf(v->rows[1], sizeof(v->rows[1]), "> %s", nm);
        item_name((menu_pos + 1) % len, nm, sizeof(nm));
        snprintf(v->rows[2], sizeof(v->rows[2]), "  %s", nm);
    }
    k_mutex_unlock(&prog_lock);
}

uint32_t prog_version(void) {
    return version;
}

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zmk/event_manager.h>
#include <zmk/events/layer_state_changed.h>

#include "prog.h"
#include "keylist.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define MENU_LAYER 3
#define MATRIX_KEYS 16
#define EV_CONFIRM 16
#define EV_NEXT 17
#define EV_PREV 18

#define GROUP_ITEMS (G_COUNT + 2)

static K_MUTEX_DEFINE(prog_lock);

static uint32_t map[PROG_KEYS];
/* 0 = wait for key, 1 = pick group, 2 = pick key inside group */
static uint8_t menu_state;
static int menu_target = -1;
static int menu_group;
static int menu_pos;
static bool menu_saved;
static uint32_t version;

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

static void save_map(const uint32_t *copy) {
    int rc = settings_save_one("np16/map", copy, sizeof(uint32_t) * PROG_KEYS);
    if (rc != 0) {
        LOG_ERR("np16 save failed: %d", rc);
    }
}

static int np16_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    const char *next;
    if (settings_name_steq(name, "map", &next) && !next) {
        if (len != sizeof(map)) {
            return -EINVAL;
        }
        int rc = read_cb(cb_arg, map, sizeof(map));
        return rc >= 0 ? 0 : rc;
    }
    return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(np16, "np16", NULL, np16_set, NULL, NULL);

static int prog_init(void) {
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

uint32_t prog_get_code(uint32_t idx) {
    if (idx >= PROG_KEYS) {
        return 0;
    }
    return map[idx];
}

void prog_menu_event(uint32_t ev) {
    uint32_t copy[PROG_KEYS];
    bool need_save = false;

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
                    map[menu_target] = 0;
                    memcpy(copy, map, sizeof(copy));
                    need_save = true;
                    menu_state = 0;
                    menu_target = -1;
                    menu_saved = true;
                } else {
                    menu_group = menu_pos - 2;
                    menu_state = 2;
                    int k = group_pos_of_code(menu_group, map[menu_target]);
                    menu_pos = (k >= 0) ? k + 1 : 1;
                }
            } else {
                if (menu_pos == 0) {
                    menu_state = 1;
                    menu_pos = menu_group + 2;
                } else {
                    const struct key_entry *e = group_item(menu_group, menu_pos - 1);
                    if (e != NULL) {
                        map[menu_target] = e->code;
                        memcpy(copy, map, sizeof(copy));
                        need_save = true;
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

    if (need_save) {
        save_map(copy);
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

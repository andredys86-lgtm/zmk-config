#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>
#include <errno.h>
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

static K_MUTEX_DEFINE(prog_lock);

static uint32_t map[PROG_KEYS];
static uint8_t menu_state;
static int menu_target = -1;
static int menu_pos;
static bool menu_saved;
static uint32_t version;

static int find_pos(uint32_t code) {
    for (size_t i = 0; i < key_list_len; i++) {
        if (key_list[i].code == code) {
            return (int)i;
        }
    }
    return 0;
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
    if (ev < MATRIX_KEYS) {
        menu_target = (int)ev;
        menu_pos = find_pos(map[ev]);
        menu_state = 1;
        menu_saved = false;
    } else if (ev == EV_CONFIRM) {
        if (menu_state == 1 && menu_target >= 0) {
            map[menu_target] = key_list[menu_pos].code;
            memcpy(copy, map, sizeof(copy));
            need_save = true;
            menu_state = 0;
            menu_target = -1;
            menu_saved = true;
        }
    } else if (ev == EV_NEXT) {
        if (menu_state == 1) {
            menu_pos = (menu_pos + 1) % (int)key_list_len;
        }
    } else if (ev == EV_PREV) {
        if (menu_state == 1) {
            menu_pos = (menu_pos + (int)key_list_len - 1) % (int)key_list_len;
        }
    }
    version++;
    k_mutex_unlock(&prog_lock);

    if (need_save) {
        save_map(copy);
    }
}

void prog_get_view(struct prog_view *v) {
    k_mutex_lock(&prog_lock, K_FOREVER);
    v->state = menu_state;
    v->target = menu_target;
    v->name = key_list[menu_pos].name;
    v->saved = menu_saved;
    k_mutex_unlock(&prog_lock);
}

uint32_t prog_version(void) {
    return version;
}

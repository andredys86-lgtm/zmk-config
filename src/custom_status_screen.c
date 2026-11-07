#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <stdio.h>
#include <lvgl.h>
#include <zmk/display/status_screen.h>
#include <zmk/battery.h>
#include <zmk/usb.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/keymap.h>
#include "calc.h"
#include "prog.h"

#define BAR_W 20
#define CALC_LAYER 1
#define PROG_LAYER 2
#define MENU_LAYER 3

static lv_obj_t *batt_icon;
static lv_obj_t *batt_pct;
static lv_obj_t *conn_icon;
static lv_obj_t *layer_num;
static lv_obj_t *calc_num;
static lv_obj_t *calc_op;
static lv_obj_t *info1;
static lv_obj_t *info2;
static lv_obj_t *info3;
static uint32_t tick;

static const char *battery_symbol(uint8_t level) {
    if (level > 85) {
        return LV_SYMBOL_BATTERY_FULL;
    }
    if (level > 60) {
        return LV_SYMBOL_BATTERY_3;
    }
    if (level > 35) {
        return LV_SYMBOL_BATTERY_2;
    }
    if (level > 10) {
        return LV_SYMBOL_BATTERY_1;
    }
    return LV_SYMBOL_BATTERY_EMPTY;
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, int y) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_width(l, BAR_W);
    lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(l, "");
    lv_obj_align(l, LV_ALIGN_TOP_RIGHT, 0, y);
    return l;
}

static lv_obj_t *make_info(lv_obj_t *parent, const lv_font_t *font, int y) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_width(l, 104);
    lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_label_set_text(l, "");
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 2, y);
    return l;
}

static void set_hidden(lv_obj_t *o, bool hide) {
    if (hide) {
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

static void refresh_cb(lv_timer_t *timer) {
    static int last_level = -1;
    static int last_charging = -1;
    static int last_usb = -1;
    static int last_layer = -1;
    static int last_hidden = -1;
    static int last_calc_shown = -1;
    static uint32_t last_calc_ver = 0xFFFFFFFF;
    static int last_prog_shown = -1;
    static int last_prog_layer = -1;
    static uint32_t last_prog_ver = 0xFFFFFFFF;

    uint8_t level = zmk_battery_state_of_charge();
    bool charging = zmk_usb_is_powered();
    bool usb = (zmk_endpoints_selected().transport == ZMK_TRANSPORT_USB);
    int layer = zmk_keymap_highest_layer_active();

    if ((int)level != last_level) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%u", level);
        lv_label_set_text(batt_pct, buf);
    }
    if ((int)level != last_level || (int)charging != last_charging) {
        lv_label_set_text(batt_icon, charging ? LV_SYMBOL_CHARGE : battery_symbol(level));
    }
    last_level = level;
    last_charging = charging;

    if ((int)usb != last_usb) {
        lv_label_set_text(conn_icon, usb ? LV_SYMBOL_USB : LV_SYMBOL_BLUETOOTH);
        last_usb = usb;
    }

    int hidden = 0;
    if (!usb && !zmk_ble_active_profile_is_connected()) {
        hidden = (tick / 5) % 2;
    }
    tick++;
    if (hidden != last_hidden) {
        set_hidden(conn_icon, hidden);
        last_hidden = hidden;
    }

    if (layer != last_layer) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", layer);
        lv_label_set_text(layer_num, buf);
        last_layer = layer;
    }

    bool on_calc = (layer == CALC_LAYER);
    if ((int)on_calc != last_calc_shown) {
        set_hidden(calc_num, !on_calc);
        set_hidden(calc_op, !on_calc);
        last_calc_shown = on_calc;
        last_calc_ver = 0xFFFFFFFF;
    }
    if (on_calc) {
        uint32_t ver = calc_version();
        if (ver != last_calc_ver) {
            char num[16];
            char op[2] = {0, 0};
            char opc = 0;
            calc_get_display(num, sizeof(num), &opc);
            op[0] = opc;
            lv_label_set_text(calc_num, num);
            lv_label_set_text(calc_op, op);
            last_calc_ver = ver;
        }
    }

    bool on_prog = (layer == PROG_LAYER || layer == MENU_LAYER);
    if ((int)on_prog != last_prog_shown) {
        set_hidden(info1, !on_prog);
        set_hidden(info2, !on_prog);
        set_hidden(info3, !on_prog);
        last_prog_shown = on_prog;
        last_prog_ver = 0xFFFFFFFF;
    }
    if (on_prog) {
        uint32_t pv = prog_version();
        if (pv != last_prog_ver || layer != last_prog_layer) {
            if (layer == PROG_LAYER) {
                lv_label_set_text(info1, "Layer 2");
                lv_label_set_text(info2, "");
                lv_label_set_text(info3, "hold: menu");
            } else {
                struct prog_view v;
                prog_get_view(&v);
                if (v.state == 0) {
                    lv_label_set_text(info1, "MENU");
                    lv_label_set_text(info2, "Press key");
                    lv_label_set_text(info3, v.saved ? "Saved!" : "hold: exit");
                } else {
                    char b[16];
                    snprintf(b, sizeof(b), "Key %d", v.target + 1);
                    lv_label_set_text(info1, b);
                    lv_label_set_text(info2, v.name);
                    lv_label_set_text(info3, "click: save");
                }
            }
            last_prog_ver = pv;
            last_prog_layer = layer;
        }
    }
}

lv_obj_t *zmk_display_status_screen() {
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    batt_icon = make_label(screen, &lv_font_montserrat_14, 0);
    batt_pct = make_label(screen, &lv_font_montserrat_10, 15);
    conn_icon = make_label(screen, &lv_font_montserrat_14, 27);
    layer_num = make_label(screen, &lv_font_montserrat_20, 41);

    calc_op = lv_label_create(screen);
    lv_obj_set_width(calc_op, 20);
    lv_obj_set_style_text_font(calc_op, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_text(calc_op, "");
    lv_obj_align(calc_op, LV_ALIGN_TOP_LEFT, 2, 2);

    calc_num = lv_label_create(screen);
    lv_obj_set_width(calc_num, 104);
    lv_obj_set_style_text_font(calc_num, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_align(calc_num, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_label_set_text(calc_num, "");
    lv_obj_align(calc_num, LV_ALIGN_TOP_LEFT, 0, 24);

    info1 = make_info(screen, &lv_font_montserrat_14, 2);
    info2 = make_info(screen, &lv_font_montserrat_20, 22);
    info3 = make_info(screen, &lv_font_montserrat_14, 48);

    refresh_cb(NULL);
    lv_timer_create(refresh_cb, 100, NULL);

    return screen;
}

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

#define BAR_W 20

static lv_obj_t *batt_icon;
static lv_obj_t *batt_pct;
static lv_obj_t *conn_icon;
static lv_obj_t *layer_num;
static uint8_t tick;

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

static void refresh_cb(lv_timer_t *timer) {
    static int last_level = -1;
    static int last_charging = -1;
    static int last_usb = -1;
    static int last_layer = -1;
    static int last_hidden = -1;

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
        hidden = (tick / 2) % 2;
    }
    tick++;
    if (hidden != last_hidden) {
        if (hidden) {
            lv_obj_add_flag(conn_icon, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(conn_icon, LV_OBJ_FLAG_HIDDEN);
        }
        last_hidden = hidden;
    }

    if (layer != last_layer) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", layer);
        lv_label_set_text(layer_num, buf);
        last_layer = layer;
    }
}

lv_obj_t *zmk_display_status_screen() {
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    batt_icon = make_label(screen, &lv_font_montserrat_14, 0);
    batt_pct = make_label(screen, &lv_font_montserrat_10, 15);
    conn_icon = make_label(screen, &lv_font_montserrat_14, 27);
    layer_num = make_label(screen, &lv_font_montserrat_20, 41);

    refresh_cb(NULL);
    lv_timer_create(refresh_cb, 250, NULL);

    return screen;
}

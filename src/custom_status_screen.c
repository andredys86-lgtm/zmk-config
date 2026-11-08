#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <stdio.h>
#include <string.h>
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

/* The OLED is inverted in the overlay, so LVGL white is a dark pixel and
 * LVGL black is a lit pixel. Menu colours are written in LVGL terms. */
#define MN_BG lv_color_white()
#define MN_FG lv_color_black()

#define SCR_W 128
#define SCR_H 64
#define MN_HINT_Y 0
#define MN_LINE1_Y 10
#define MN_GRID_Y 12
#define MN_CELL_H 10
#define MN_LINE2_Y 54
#define MN_SEQ_Y 55

static lv_obj_t *batt_icon;
static lv_obj_t *batt_pct;
static lv_obj_t *conn_icon;
static lv_obj_t *layer_num;
static lv_obj_t *calc_num;
static lv_obj_t *calc_op;
static lv_obj_t *info_t;
static lv_obj_t *info_a;
static lv_obj_t *info_b;
static lv_obj_t *info_c;
static uint32_t tick;

/* menu objects */
static lv_obj_t *mn_root;
static lv_obj_t *mn_hint;
static lv_obj_t *mn_seq;
static lv_obj_t *mn_center;
static lv_obj_t *mn_msg;
static lv_obj_t *mn_cell[PROG_MENU_MAX_CELLS];
static bool cell_sel[PROG_MENU_MAX_CELLS];

static lv_style_t st_root;
static lv_style_t st_line;
static lv_style_t st_cell;
static lv_style_t st_cell_sel;
static lv_style_t st_txt;
static lv_style_t st_center;
static lv_style_t st_msg;

static struct prog_menu_view mv;
static bool menu_force = true;
static uint32_t last_mv_ver = 0xFFFFFFFF;
static int last_mv_msg = -1;
static int last_in_menu = 0;

/* status caches */
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

static void set_bar_hidden(bool hide) {
    set_hidden(batt_icon, hide);
    set_hidden(batt_pct, hide);
    set_hidden(conn_icon, hide);
    set_hidden(layer_num, hide);
}

static void invalidate_status_cache(void) {
    last_level = -1;
    last_charging = -1;
    last_usb = -1;
    last_layer = -1;
    last_hidden = -1;
    last_calc_shown = -1;
    last_calc_ver = 0xFFFFFFFF;
    last_prog_shown = -1;
    last_prog_layer = -1;
    last_prog_ver = 0xFFFFFFFF;
}

/* ---------- menu ---------- */

static void init_styles(void) {
    lv_style_init(&st_root);
    lv_style_set_bg_opa(&st_root, LV_OPA_TRANSP);
    lv_style_set_border_width(&st_root, 0);
    lv_style_set_pad_all(&st_root, 0);
    lv_style_set_radius(&st_root, 0);

    lv_style_init(&st_line);
    lv_style_set_bg_opa(&st_line, LV_OPA_COVER);
    lv_style_set_bg_color(&st_line, MN_FG);
    lv_style_set_border_width(&st_line, 0);
    lv_style_set_radius(&st_line, 0);

    lv_style_init(&st_cell);
    lv_style_set_bg_opa(&st_cell, LV_OPA_COVER);
    lv_style_set_bg_color(&st_cell, MN_BG);
    lv_style_set_border_width(&st_cell, 1);
    lv_style_set_border_color(&st_cell, MN_FG);
    lv_style_set_radius(&st_cell, 0);
    lv_style_set_pad_all(&st_cell, 0);
    lv_style_set_text_color(&st_cell, MN_FG);
    lv_style_set_text_font(&st_cell, &lv_font_montserrat_8);
    lv_style_set_text_align(&st_cell, LV_TEXT_ALIGN_CENTER);

    lv_style_init(&st_cell_sel);
    lv_style_set_bg_color(&st_cell_sel, MN_FG);
    lv_style_set_text_color(&st_cell_sel, MN_BG);

    lv_style_init(&st_txt);
    lv_style_set_text_color(&st_txt, MN_FG);
    lv_style_set_text_font(&st_txt, &lv_font_montserrat_8);

    lv_style_init(&st_center);
    lv_style_set_text_color(&st_center, MN_FG);
    lv_style_set_text_font(&st_center, &lv_font_montserrat_14);
    lv_style_set_text_align(&st_center, LV_TEXT_ALIGN_CENTER);

    lv_style_init(&st_msg);
    lv_style_set_bg_opa(&st_msg, LV_OPA_COVER);
    lv_style_set_bg_color(&st_msg, MN_FG);
    lv_style_set_text_color(&st_msg, MN_BG);
    lv_style_set_text_font(&st_msg, &lv_font_montserrat_14);
    lv_style_set_pad_hor(&st_msg, 8);
    lv_style_set_pad_ver(&st_msg, 3);
    lv_style_set_radius(&st_msg, 0);
}

static lv_obj_t *make_line(lv_obj_t *parent, int y) {
    lv_obj_t *ln = lv_obj_create(parent);
    lv_obj_remove_style_all(ln);
    lv_obj_set_pos(ln, 0, y);
    lv_obj_set_size(ln, SCR_W, 1);
    lv_obj_add_style(ln, &st_line, LV_PART_MAIN);
    return ln;
}

static void build_menu(lv_obj_t *screen) {
    init_styles();

    mn_root = lv_obj_create(screen);
    lv_obj_remove_style_all(mn_root);
    lv_obj_set_pos(mn_root, 0, 0);
    lv_obj_set_size(mn_root, SCR_W, SCR_H);
    lv_obj_clear_flag(mn_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_style(mn_root, &st_root, LV_PART_MAIN);

    mn_hint = lv_label_create(mn_root);
    lv_obj_remove_style_all(mn_hint);
    lv_obj_add_style(mn_hint, &st_txt, LV_PART_MAIN);
    lv_obj_set_style_text_align(mn_hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(mn_hint, LV_LABEL_LONG_CLIP);
    lv_obj_set_size(mn_hint, SCR_W, 10);
    lv_obj_set_pos(mn_hint, 0, MN_HINT_Y);
    lv_label_set_text(mn_hint, "");

    make_line(mn_root, MN_LINE1_Y);
    make_line(mn_root, MN_LINE2_Y);

    for (int i = 0; i < PROG_MENU_MAX_CELLS; i++) {
        lv_obj_t *c = lv_label_create(mn_root);
        lv_obj_remove_style_all(c);
        lv_obj_add_style(c, &st_cell, LV_PART_MAIN);
        lv_label_set_long_mode(c, LV_LABEL_LONG_CLIP);
        lv_label_set_text(c, "");
        lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
        mn_cell[i] = c;
        cell_sel[i] = false;
    }

    mn_seq = lv_label_create(mn_root);
    lv_obj_remove_style_all(mn_seq);
    lv_obj_add_style(mn_seq, &st_txt, LV_PART_MAIN);
    lv_label_set_long_mode(mn_seq, LV_LABEL_LONG_CLIP);
    lv_obj_set_size(mn_seq, SCR_W, 9);
    lv_obj_set_pos(mn_seq, 0, MN_SEQ_Y);
    lv_label_set_text(mn_seq, "");

    mn_center = lv_label_create(mn_root);
    lv_obj_remove_style_all(mn_center);
    lv_obj_add_style(mn_center, &st_center, LV_PART_MAIN);
    lv_label_set_long_mode(mn_center, LV_LABEL_LONG_CLIP);
    lv_obj_set_size(mn_center, SCR_W, 18);
    lv_obj_set_pos(mn_center, 0, 24);
    lv_label_set_text(mn_center, "Press key");
    lv_obj_add_flag(mn_center, LV_OBJ_FLAG_HIDDEN);

    mn_msg = lv_label_create(mn_root);
    lv_obj_remove_style_all(mn_msg);
    lv_obj_add_style(mn_msg, &st_msg, LV_PART_MAIN);
    lv_label_set_text(mn_msg, "");
    lv_obj_align(mn_msg, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(mn_msg, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_flag(mn_root, LV_OBJ_FLAG_HIDDEN);
}

/* Shows the tail of the sequence that fits the row, with ".." when the
 * beginning is hidden. */
static void seq_tail(const char *full, char *out, size_t n) {
    const lv_font_t *f = &lv_font_montserrat_8;

    if (lv_txt_get_width(full, strlen(full), f, 0, 0) <= SCR_W - 1) {
        snprintf(out, n, "%s", full);
        return;
    }
    const char *p = full;
    while (true) {
        const char *sp = strchr(p, ' ');
        if (sp == NULL) {
            break;
        }
        p = sp + 1;
        snprintf(out, n, ".. %s", p);
        if (lv_txt_get_width(out, strlen(out), f, 0, 0) <= SCR_W - 1) {
            return;
        }
    }
    snprintf(out, n, ".. %s", p);
}

static void menu_apply(const struct prog_menu_view *v) {
    static char tail[128];

    lv_label_set_text(mn_hint, v->hint);

    int cols = v->cols > 0 ? v->cols : 3;
    int cw = 127 / cols;

    for (int i = 0; i < PROG_MENU_MAX_CELLS; i++) {
        lv_obj_t *c = mn_cell[i];
        if (i >= v->cells) {
            set_hidden(c, true);
            continue;
        }
        int col = i % cols;
        int row = i / cols;
        lv_obj_set_pos(c, col * cw, MN_GRID_Y + row * MN_CELL_H);
        lv_obj_set_size(c, cw + 1, MN_CELL_H + 1);
        lv_label_set_text(c, v->cell[i]);
        bool sel = (i == v->cursor);
        if (sel != cell_sel[i]) {
            if (sel) {
                lv_obj_add_style(c, &st_cell_sel, LV_PART_MAIN);
            } else {
                lv_obj_remove_style(c, &st_cell_sel, LV_PART_MAIN);
            }
            cell_sel[i] = sel;
        }
        set_hidden(c, false);
    }

    set_hidden(mn_center, v->mode != PROG_MODE_PICK);

    seq_tail(v->seq, tail, sizeof(tail));
    lv_label_set_text(mn_seq, tail);

    if (v->msg_kind == PROG_MSG_SAVED) {
        lv_label_set_text(mn_msg, "SAVED");
    } else if (v->msg_kind == PROG_MSG_CANCELLED) {
        lv_label_set_text(mn_msg, "CANCELLED");
    } else if (v->msg_kind == PROG_MSG_FULL) {
        lv_label_set_text(mn_msg, "FULL");
    }
    set_hidden(mn_msg, v->msg_kind == PROG_MSG_NONE);
    lv_obj_align(mn_msg, LV_ALIGN_CENTER, 0, 0);
}

static void menu_refresh(void) {
    prog_get_menu_view(&mv);
    if (!menu_force && mv.version == last_mv_ver && (int)mv.msg_kind == last_mv_msg) {
        return;
    }
    menu_force = false;
    last_mv_ver = mv.version;
    last_mv_msg = mv.msg_kind;
    menu_apply(&mv);
}

/* ---------- status screen ---------- */

static void refresh_cb(lv_timer_t *timer) {
    uint8_t level = zmk_battery_state_of_charge();
    bool charging = zmk_usb_is_powered();
    bool usb = (zmk_endpoints_selected().transport == ZMK_TRANSPORT_USB);
    int layer = zmk_keymap_highest_layer_active();
    bool in_menu = (layer == MENU_LAYER);

    if ((int)in_menu != last_in_menu) {
        last_in_menu = in_menu;
        set_hidden(mn_root, !in_menu);
        set_bar_hidden(in_menu);
        if (in_menu) {
            set_hidden(calc_num, true);
            set_hidden(calc_op, true);
            set_hidden(info_t, true);
            set_hidden(info_a, true);
            set_hidden(info_b, true);
            set_hidden(info_c, true);
            menu_force = true;
        } else {
            invalidate_status_cache();
        }
    }

    if (in_menu) {
        menu_refresh();
        return;
    }

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

    bool on_prog = (layer == PROG_LAYER);
    if ((int)on_prog != last_prog_shown) {
        set_hidden(info_t, !on_prog);
        set_hidden(info_a, !on_prog);
        set_hidden(info_b, !on_prog);
        set_hidden(info_c, !on_prog);
        last_prog_shown = on_prog;
        last_prog_ver = 0xFFFFFFFF;
    }
    if (on_prog) {
        uint32_t pv = prog_version();
        if (pv != last_prog_ver || layer != last_prog_layer) {
            lv_label_set_text(info_t, "PROG");
            lv_label_set_text(info_a, "");
            lv_label_set_text(info_b, "Layer 2");
            lv_label_set_text(info_c, "hold: menu");
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

    info_t = make_info(screen, &lv_font_montserrat_10, 0);
    info_a = make_info(screen, &lv_font_montserrat_14, 13);
    info_b = make_info(screen, &lv_font_montserrat_14, 29);
    info_c = make_info(screen, &lv_font_montserrat_14, 45);

    build_menu(screen);

    refresh_cb(NULL);
    lv_timer_create(refresh_cb, 100, NULL);

    return screen;
}

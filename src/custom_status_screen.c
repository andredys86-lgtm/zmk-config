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
#define MN_SEQ_Y 47
#define MN_SEQ_H 17
#define MN_GRID_H 46

/* calculator layout */
#define EXPR_X 2
#define EXPR_W 124
#define RES_W 118

static lv_obj_t *batt_icon;
static lv_obj_t *batt_pct;
static lv_obj_t *conn_icon;
static lv_obj_t *layer_num;
static lv_obj_t *calc_expr;
static lv_obj_t *calc_res;
static lv_obj_t *info_t;
static lv_obj_t *info_a;
static lv_obj_t *info_b;
static lv_obj_t *info_c;
static uint32_t tick;

/* menu objects */
static lv_obj_t *mn_root;
static lv_obj_t *mn_seq;
static lv_obj_t *mn_center;
static lv_obj_t *mn_msg;
static lv_obj_t *mn_cell[PROG_MENU_MAX_CELLS];
static bool cell_sel[PROG_MENU_MAX_CELLS];
static bool cell_hl[PROG_MENU_MAX_CELLS];

static lv_style_t st_root;
static lv_style_t st_cell;
static lv_style_t st_cell_sel;
static lv_style_t st_cell_hl;
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
static int last_prog_shown = -1;
static int last_prog_layer = -1;
static uint32_t last_prog_ver = 0xFFFFFFFF;

/* calculator view cache */
static struct calc_view cv;
static struct calc_view last_cv;
static bool calc_force = true;
static char expr_text[CALC_DISP_SZ];

/* scratch buffers for the layout code, protected by lay_lock */
static K_MUTEX_DEFINE(lay_lock);
static char lay_out[CALC_DISP_SZ];
static char lay_test[400];
static char lay_line[256];
static char lay_chunk[128];

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
    calc_force = true;
    last_prog_shown = -1;
    last_prog_layer = -1;
    last_prog_ver = 0xFFFFFFFF;
}

/* ---------- calculator ---------- */

/* index 0: Montserrat 20 (1 line), 1: Montserrat 14 (2 lines), 2: Montserrat 10 (3 lines) */
static const lv_font_t *font_by_idx(int i) {
    if (i == 0) {
        return &lv_font_montserrat_20;
    }
    if (i == 1) {
        return &lv_font_montserrat_14;
    }
    return &lv_font_montserrat_10;
}

/* Wraps the expression for one font. Wrap points are CALC_MARK characters.
 * Strict mode fails when the text does not fit; soft mode truncates instead. */
static bool layout_try(const char *disp, int fi, bool soft, char *out, size_t outn, int *nl) {
    const lv_font_t *f = font_by_idx(fi);
    int maxl = fi + 1;
    int lines = 1;
    size_t o = 0;
    int ll = 0;
    int cl = 0;
    const char *p = disp;

    lay_line[0] = '\0';
    out[0] = '\0';

    for (;;) {
        char ch = *p;
        if (ch != '\0' && ch != CALC_MARK) {
            if (cl < (int)sizeof(lay_chunk) - 1) {
                lay_chunk[cl++] = ch;
            } else if (!soft) {
                return false;
            }
            p++;
            continue;
        }
        lay_chunk[cl] = '\0';
        if (cl > 0) {
            snprintf(lay_test, sizeof(lay_test), "%s%s", lay_line, lay_chunk);
            if (lv_txt_get_width(lay_test, strlen(lay_test), f, 0, 0) > EXPR_W) {
                if (ll > 0) {
                    if (lines >= maxl) {
                        if (soft) {
                            goto finish;
                        }
                        return false;
                    }
                    lines++;
                    if (o + 1 < outn) {
                        out[o++] = '\n';
                    }
                    lay_line[0] = '\0';
                    ll = 0;
                }
                if (!soft && lv_txt_get_width(lay_chunk, cl, f, 0, 0) > EXPR_W) {
                    return false;
                }
            }
            if (o + (size_t)cl + 1 >= outn) {
                if (soft) {
                    goto finish;
                }
                return false;
            }
            memcpy(out + o, lay_chunk, cl);
            o += cl;
            if (ll + cl < (int)sizeof(lay_line)) {
                memcpy(lay_line + ll, lay_chunk, cl);
                ll += cl;
                lay_line[ll] = '\0';
            }
        }
        cl = 0;
        if (ch == '\0') {
            break;
        }
        p++;
    }
finish:
    out[o] = '\0';
    *nl = lines;
    return true;
}

bool calc_layout_fits(const char *disp) {
    bool ok = false;
    int nl = 1;
    k_mutex_lock(&lay_lock, K_FOREVER);
    for (int fi = 0; fi < 3 && !ok; fi++) {
        ok = layout_try(disp, fi, false, lay_out, sizeof(lay_out), &nl);
    }
    k_mutex_unlock(&lay_lock);
    return ok;
}

bool calc_result_fits(const char *res) {
    return lv_txt_get_width(res, strlen(res), &lv_font_montserrat_10, 0, 0) <= RES_W;
}

static void calc_view_apply(const struct calc_view *v) {
    int fi;
    int nl = 1;
    bool ok = false;

    k_mutex_lock(&lay_lock, K_FOREVER);
    for (fi = 0; fi < 3; fi++) {
        if (layout_try(v->disp, fi, false, expr_text, sizeof(expr_text) - 2, &nl)) {
            ok = true;
            break;
        }
    }
    if (!ok) {
        fi = 2;
        layout_try(v->disp, fi, true, expr_text, sizeof(expr_text) - 2, &nl);
    }
    k_mutex_unlock(&lay_lock);

    if (v->cursor) {
        size_t l = strlen(expr_text);
        expr_text[l] = '_';
        expr_text[l + 1] = '\0';
    }
    lv_obj_set_style_text_font(calc_expr, font_by_idx(fi), LV_PART_MAIN);
    lv_label_set_text(calc_expr, expr_text);
    lv_obj_set_pos(calc_expr, EXPR_X, nl >= 3 ? 0 : 6);

    int rf = 0;
    while (rf < 2 && lv_txt_get_width(v->res, strlen(v->res), font_by_idx(rf), 0, 0) > RES_W) {
        rf++;
    }
    const lv_font_t *f = font_by_idx(rf);
    lv_obj_set_style_text_font(calc_res, f, LV_PART_MAIN);
    lv_label_set_text(calc_res, v->res);
    lv_obj_set_pos(calc_res, 0, SCR_H - 6 - lv_font_get_line_height(f));
}

/* ---------- menu ---------- */

static void init_styles(void) {
    lv_style_init(&st_root);
    lv_style_set_bg_opa(&st_root, LV_OPA_TRANSP);
    lv_style_set_border_width(&st_root, 0);
    lv_style_set_pad_all(&st_root, 0);
    lv_style_set_radius(&st_root, 0);

    lv_style_init(&st_cell);
    lv_style_set_bg_opa(&st_cell, LV_OPA_COVER);
    lv_style_set_bg_color(&st_cell, MN_BG);
    lv_style_set_border_width(&st_cell, 1);
    lv_style_set_border_color(&st_cell, MN_FG);
    lv_style_set_radius(&st_cell, 0);
    lv_style_set_pad_all(&st_cell, 0);
    lv_style_set_text_color(&st_cell, MN_FG);
    lv_style_set_text_align(&st_cell, LV_TEXT_ALIGN_CENTER);

    lv_style_init(&st_cell_sel);
    lv_style_set_bg_color(&st_cell_sel, MN_FG);
    lv_style_set_text_color(&st_cell_sel, MN_BG);

    lv_style_init(&st_cell_hl);
    lv_style_set_border_width(&st_cell_hl, 2);

    lv_style_init(&st_txt);
    lv_style_set_text_color(&st_txt, MN_FG);
    lv_style_set_text_font(&st_txt, &lv_font_montserrat_14);

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

static void build_menu(lv_obj_t *screen) {
    init_styles();

    mn_root = lv_obj_create(screen);
    lv_obj_remove_style_all(mn_root);
    lv_obj_set_pos(mn_root, 0, 0);
    lv_obj_set_size(mn_root, SCR_W, SCR_H);
    lv_obj_clear_flag(mn_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_style(mn_root, &st_root, LV_PART_MAIN);

    for (int i = 0; i < PROG_MENU_MAX_CELLS; i++) {
        lv_obj_t *c = lv_label_create(mn_root);
        lv_obj_remove_style_all(c);
        lv_obj_add_style(c, &st_cell, LV_PART_MAIN);
        lv_obj_set_style_text_font(c, &lv_font_montserrat_10, LV_PART_MAIN);
        lv_label_set_long_mode(c, LV_LABEL_LONG_CLIP);
        lv_label_set_text(c, "");
        lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
        mn_cell[i] = c;
        cell_sel[i] = false;
        cell_hl[i] = false;
    }

    mn_seq = lv_label_create(mn_root);
    lv_obj_remove_style_all(mn_seq);
    lv_obj_add_style(mn_seq, &st_txt, LV_PART_MAIN);
    lv_label_set_long_mode(mn_seq, LV_LABEL_LONG_CLIP);
    lv_obj_set_size(mn_seq, SCR_W, MN_SEQ_H);
    lv_obj_set_pos(mn_seq, 0, MN_SEQ_Y);
    lv_label_set_text(mn_seq, "");

    mn_center = lv_label_create(mn_root);
    lv_obj_remove_style_all(mn_center);
    lv_obj_add_style(mn_center, &st_center, LV_PART_MAIN);
    lv_label_set_long_mode(mn_center, LV_LABEL_LONG_CLIP);
    lv_obj_set_size(mn_center, SCR_W, 18);
    lv_obj_set_pos(mn_center, 0, 22);
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
    const lv_font_t *f = &lv_font_montserrat_14;

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
    static char tail[208];

    int cols = v->cols > 0 ? v->cols : 3;
    int rows = v->rows > 0 ? v->rows : 4;
    int cw = 127 / cols;
    int ch = (v->font == 14) ? 17 : 12;
    if (rows * ch + 1 > MN_GRID_H) {
        ch = (MN_GRID_H - 1) / rows;
    }
    const lv_font_t *font = (v->font == 14) ? &lv_font_montserrat_14 : &lv_font_montserrat_10;

    for (int i = 0; i < PROG_MENU_MAX_CELLS; i++) {
        lv_obj_t *c = mn_cell[i];
        if (i >= v->cells) {
            set_hidden(c, true);
            continue;
        }
        int col = i % cols;
        int row = i / cols;
        lv_obj_set_pos(c, col * cw, row * ch);
        lv_obj_set_size(c, cw + 1, ch + 1);
        lv_obj_set_style_text_font(c, font, LV_PART_MAIN);
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
        bool hl = ((v->hl_mask >> i) & 1u) != 0;
        if (hl != cell_hl[i]) {
            if (hl) {
                lv_obj_add_style(c, &st_cell_hl, LV_PART_MAIN);
            } else {
                lv_obj_remove_style(c, &st_cell_hl, LV_PART_MAIN);
            }
            cell_hl[i] = hl;
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
    bool on_calc = (layer == CALC_LAYER);

    if ((int)in_menu != last_in_menu) {
        last_in_menu = in_menu;
        set_hidden(mn_root, !in_menu);
        set_bar_hidden(in_menu);
        if (in_menu) {
            set_hidden(calc_expr, true);
            set_hidden(calc_res, true);
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

    if ((int)on_calc != last_calc_shown) {
        set_hidden(calc_expr, !on_calc);
        set_hidden(calc_res, !on_calc);
        set_bar_hidden(on_calc);
        last_calc_shown = on_calc;
        calc_force = true;
        if (!on_calc) {
            last_level = -1;
            last_charging = -1;
            last_usb = -1;
            last_layer = -1;
            last_hidden = -1;
        }
    }

    if (!on_calc) {
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
    }

    if (on_calc) {
        calc_get_view(&cv);
        if (calc_force || strcmp(cv.disp, last_cv.disp) != 0 || strcmp(cv.res, last_cv.res) != 0 ||
            cv.cursor != last_cv.cursor) {
            calc_view_apply(&cv);
            last_cv = cv;
            calc_force = false;
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

    calc_expr = lv_label_create(screen);
    lv_obj_set_width(calc_expr, EXPR_W + 2);
    lv_label_set_long_mode(calc_expr, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(calc_expr, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_label_set_text(calc_expr, "");
    lv_obj_set_pos(calc_expr, EXPR_X, 6);

    calc_res = lv_label_create(screen);
    lv_obj_set_width(calc_res, RES_W);
    lv_label_set_long_mode(calc_res, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(calc_res, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_set_style_text_font(calc_res, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_label_set_text(calc_res, "");
    lv_obj_set_pos(calc_res, 0, SCR_H - 6 - 20);

    info_t = make_info(screen, &lv_font_montserrat_10, 0);
    info_a = make_info(screen, &lv_font_montserrat_14, 13);
    info_b = make_info(screen, &lv_font_montserrat_14, 29);
    info_c = make_info(screen, &lv_font_montserrat_14, 45);

    build_menu(screen);

    refresh_cb(NULL);
    lv_timer_create(refresh_cb, 100, NULL);

    return screen;
}

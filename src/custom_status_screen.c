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
#include <zmk/activity.h>
#include "calc.h"
#include "prog.h"

#define BAR_W 20
#define CALC_LAYER 1
#define PROG_LAYER 2
#define MENU_LAYER 3
/* reduced mode after a long inactivity: only the battery icon is shown */
#define MODE_IDLE 4

/* The OLED is inverted in the overlay, so LVGL white is a dark pixel and
 * LVGL black is a lit pixel. Menu colours are written in LVGL terms. */
#define MN_BG lv_color_white()
#define MN_FG lv_color_black()

#define SCR_W 128
#define SCR_H 64
#define MN_SEQ_Y 47
#define MN_SEQ_H 17
#define MN_GRID_H 46

/* key list (menu start screen) */
#define PICK_KEYS 16
#define PICK_ROWS 5
#define PICK_RH 12

/* layer 2 grid */
#define PG_CW 32
#define PG_CH 16

/* right bar: connection icon, percent, vertical battery with 10 segments */
#define BT_X (SCR_W - BAR_W)
#define BT_Y 28
#define BT_W 20
#define BT_H 36
#define BT_SEGS 10

/* calculator layout */
#define EXPR_X 2
#define EXPR_W 124
#define RES_W 118

/* calculator function menu: 5 columns x 2 rows at the bottom */
#define CM_COLS 5
#define CM_X0 2
#define CM_W 124
#define CM_H 13
#define CM_Y0 (SCR_H - 2 * CM_H - 1)

static lv_obj_t *batt_pct;
static lv_obj_t *conn_icon;
static lv_obj_t *bt_box;
static lv_obj_t *bt_seg[BT_SEGS]; /* 0 = bottom segment */
static lv_obj_t *bt_chg;
static lv_obj_t *kb_icon;
static lv_obj_t *calc_expr;
static lv_obj_t *calc_res;
static uint32_t tick;

/* layer 2 grid */
static lv_obj_t *pg_cell[16];

/* menu objects */
static lv_obj_t *mn_root;
static lv_obj_t *mn_seq;
static lv_obj_t *mn_msg;
static lv_obj_t *mn_cell[PROG_MENU_MAX_CELLS];
static bool cell_sel[PROG_MENU_MAX_CELLS];
static bool cell_hl[PROG_MENU_MAX_CELLS];

/* calculator menu objects */
static const char *const cm_label[CALC_MENU_CELLS] = {"DEL", "(",  ")",   "SQ", "X^Y",
                                                      "PI",  "%",  "+/-", "AC", "EXIT"};
static lv_obj_t *cm_cell[CALC_MENU_CELLS];
static bool cm_sel[CALC_MENU_CELLS];

static lv_style_t st_root;
static lv_style_t st_cell;
static lv_style_t st_cell_sel;
static lv_style_t st_cell_hl;
static lv_style_t st_txt;
static lv_style_t st_msg;

static struct prog_menu_view mv;
static bool menu_force = true;
static uint32_t last_mv_ver = 0xFFFFFFFF;
static int last_mv_msg = -1;

/* status caches */
static int last_mode = -1;
static int last_level = -1;
static int last_charging = -1;
static int last_usb = -1;
static int last_hidden = -1;
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

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, int y) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_width(l, BAR_W);
    lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(l, "");
    lv_obj_align(l, LV_ALIGN_TOP_RIGHT, 0, y);
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
    set_hidden(batt_pct, hide);
    set_hidden(conn_icon, hide);
    set_hidden(bt_box, hide);
}

static void invalidate_status_cache(void) {
    last_level = -1;
    last_charging = -1;
    last_usb = -1;
    last_hidden = -1;
    calc_force = true;
    last_prog_ver = 0xFFFFFFFF;
}

/* ---------- right bar: vertical battery ---------- */

static lv_obj_t *make_rect(lv_obj_t *parent, int x, int y, int w, int h, bool fill) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, 0, LV_PART_MAIN);
    if (fill) {
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(o, MN_FG, LV_PART_MAIN);
    } else {
        lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(o, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(o, MN_FG, LV_PART_MAIN);
        lv_obj_set_style_border_opa(o, LV_OPA_COVER, LV_PART_MAIN);
    }
    return o;
}

static void build_battery(lv_obj_t *screen) {
    bt_box = lv_obj_create(screen);
    lv_obj_remove_style_all(bt_box);
    lv_obj_clear_flag(bt_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(bt_box, BT_X, BT_Y);
    lv_obj_set_size(bt_box, BT_W, BT_H);

    make_rect(bt_box, 7, 0, 6, 3, true);    /* contact */
    make_rect(bt_box, 3, 3, 14, 33, false); /* body, inner height 31 */
    /* 10 segments, 2 px high with a 1 px gap */
    for (int i = 0; i < BT_SEGS; i++) {
        bt_seg[i] = make_rect(bt_box, 5, 5 + 3 * (BT_SEGS - 1 - i), 10, 2, true);
    }

    bt_chg = lv_label_create(bt_box);
    lv_obj_set_width(bt_chg, 14);
    lv_obj_set_style_text_font(bt_chg, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(bt_chg, MN_FG, LV_PART_MAIN);
    lv_obj_set_style_text_align(bt_chg, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(bt_chg, LV_SYMBOL_CHARGE);
    lv_obj_set_pos(bt_chg, 3, 3 + (33 - lv_font_get_line_height(&lv_font_montserrat_14)) / 2);
    lv_obj_add_flag(bt_chg, LV_OBJ_FLAG_HIDDEN);
}

/* 10 segments of 10 percent each: 91-100 ten, 81-90 nine, ... 1-10 one, 0 none */
static void batt_draw(uint8_t level, bool charging) {
    int segs = (level + 9) / 10;
    if (segs > BT_SEGS) {
        segs = BT_SEGS;
    }
    for (int i = 0; i < BT_SEGS; i++) {
        set_hidden(bt_seg[i], charging || i >= segs);
    }
    set_hidden(bt_chg, !charging);
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

static void cm_update(bool show, int sel) {
    for (int i = 0; i < CALC_MENU_CELLS; i++) {
        set_hidden(cm_cell[i], !show);
        if (!show) {
            continue;
        }
        bool s = (i == sel);
        if (s != cm_sel[i]) {
            if (s) {
                lv_obj_add_style(cm_cell[i], &st_cell_sel, LV_PART_MAIN);
            } else {
                lv_obj_remove_style(cm_cell[i], &st_cell_sel, LV_PART_MAIN);
            }
            cm_sel[i] = s;
        }
    }
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
    lv_obj_set_pos(calc_expr, EXPR_X, 0);

    int rf = 0;
    while (rf < 2 && lv_txt_get_width(v->res, strlen(v->res), font_by_idx(rf), 0, 0) > RES_W) {
        rf++;
    }
    const lv_font_t *f = font_by_idx(rf);
    lv_obj_set_style_text_font(calc_res, f, LV_PART_MAIN);
    lv_label_set_text(calc_res, v->res);
    lv_obj_set_pos(calc_res, 0, SCR_H - 6 - lv_font_get_line_height(f));

    set_hidden(calc_res, v->menu_open);
    cm_update(v->menu_open, v->menu_sel);
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

    mn_msg = lv_label_create(mn_root);
    lv_obj_remove_style_all(mn_msg);
    lv_obj_add_style(mn_msg, &st_msg, LV_PART_MAIN);
    lv_label_set_text(mn_msg, "");
    lv_obj_align(mn_msg, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(mn_msg, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_flag(mn_root, LV_OBJ_FLAG_HIDDEN);
}

/* Calculator function menu cells (created after init_styles). */
static void build_calc_menu(lv_obj_t *screen) {
    for (int i = 0; i < CALC_MENU_CELLS; i++) {
        int col = i % CM_COLS;
        int row = i / CM_COLS;
        int x1 = CM_X0 + (col * CM_W) / CM_COLS;
        int x2 = CM_X0 + ((col + 1) * CM_W) / CM_COLS;

        lv_obj_t *c = lv_label_create(screen);
        lv_obj_remove_style_all(c);
        lv_obj_add_style(c, &st_cell, LV_PART_MAIN);
        lv_obj_set_style_text_font(c, &lv_font_montserrat_10, LV_PART_MAIN);
        lv_label_set_long_mode(c, LV_LABEL_LONG_CLIP);
        lv_label_set_text(c, cm_label[i]);
        lv_obj_set_pos(c, x1, CM_Y0 + row * CM_H);
        lv_obj_set_size(c, x2 - x1 + 1, CM_H + 1);
        lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
        cm_cell[i] = c;
        cm_sel[i] = false;
    }
}

/* Layer 2: 4 x 4 frames, one per programmable key (created after init_styles). */
static void build_prog_grid(lv_obj_t *screen) {
    const lv_font_t *f = &lv_font_montserrat_10;
    int padt = (PG_CH - lv_font_get_line_height(f)) / 2 - 1;
    if (padt < 0) {
        padt = 0;
    }

    for (int i = 0; i < 16; i++) {
        int col = i % 4;
        int row = i / 4;
        lv_obj_t *c = lv_label_create(screen);
        lv_obj_remove_style_all(c);
        lv_obj_add_style(c, &st_cell, LV_PART_MAIN);
        lv_obj_set_style_text_font(c, f, LV_PART_MAIN);
        lv_obj_set_style_pad_top(c, padt, LV_PART_MAIN);
        lv_obj_set_style_pad_left(c, 1, LV_PART_MAIN);
        lv_label_set_long_mode(c, LV_LABEL_LONG_CLIP);
        lv_label_set_text(c, "");
        lv_obj_set_pos(c, col * PG_CW, row * PG_CH);
        lv_obj_set_size(c, PG_CW + (col < 3 ? 1 : 0), PG_CH + (row < 3 ? 1 : 0));
        lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
        pg_cell[i] = c;
    }
}

static void pg_update(void) {
    static char t[48];
    const lv_font_t *f = &lv_font_montserrat_10;

    for (int i = 0; i < 16; i++) {
        prog_key_text(i, t, sizeof(t));
        lv_label_set_text(pg_cell[i], t);
        int w = lv_txt_get_width(t, strlen(t), f, 0, 0);
        lv_obj_set_style_text_align(pg_cell[i],
                                    (w <= PG_CW - 4) ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_LEFT,
                                    LV_PART_MAIN);
    }
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

static void cell_state(int i, bool sel, bool hl) {
    lv_obj_t *c = mn_cell[i];
    if (sel != cell_sel[i]) {
        if (sel) {
            lv_obj_add_style(c, &st_cell_sel, LV_PART_MAIN);
        } else {
            lv_obj_remove_style(c, &st_cell_sel, LV_PART_MAIN);
        }
        cell_sel[i] = sel;
    }
    if (hl != cell_hl[i]) {
        if (hl) {
            lv_obj_add_style(c, &st_cell_hl, LV_PART_MAIN);
        } else {
            lv_obj_remove_style(c, &st_cell_hl, LV_PART_MAIN);
        }
        cell_hl[i] = hl;
    }
}

static void menu_apply(const struct prog_menu_view *v) {
    static char tail[208];
    static char tmp[40];
    static char rowtxt[56];

    if (v->mode == PROG_MODE_PICK) {
        /* key list: number and assigned combination, scrolled by the encoder */
        int cur = v->cursor;
        int top = cur - PICK_ROWS / 2;
        if (top < 0) {
            top = 0;
        }
        if (top > PICK_KEYS - PICK_ROWS) {
            top = PICK_KEYS - PICK_ROWS;
        }
        for (int i = 0; i < PROG_MENU_MAX_CELLS; i++) {
            lv_obj_t *c = mn_cell[i];
            if (i >= PICK_ROWS) {
                set_hidden(c, true);
                continue;
            }
            int k = top + i;
            prog_key_text(k, tmp, sizeof(tmp));
            snprintf(rowtxt, sizeof(rowtxt), "%d  %s", k + 1, tmp[0] ? tmp : "-");
            lv_obj_set_pos(c, 0, i * PICK_RH);
            lv_obj_set_size(c, SCR_W, PICK_RH + 1);
            lv_obj_set_style_text_font(c, &lv_font_montserrat_10, LV_PART_MAIN);
            lv_obj_set_style_text_align(c, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
            lv_obj_set_style_pad_left(c, 3, LV_PART_MAIN);
            lv_label_set_text(c, rowtxt);
            cell_state(i, k == cur, false);
            set_hidden(c, false);
        }
        set_hidden(mn_seq, true);
    } else {
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
            lv_obj_set_style_text_align(c, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
            lv_obj_set_style_pad_left(c, 0, LV_PART_MAIN);
            lv_label_set_text(c, v->cell[i]);
            cell_state(i, i == v->cursor, ((v->hl_mask >> i) & 1u) != 0);
            set_hidden(c, false);
        }

        set_hidden(mn_seq, false);
        seq_tail(v->seq, tail, sizeof(tail));
        lv_label_set_text(mn_seq, tail);
    }

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

/* Shows only what belongs to the mode: 0 bar + keyboard icon, 1 calculator,
 * 2 key grid, 3 menu, MODE_IDLE only the battery icon. */
static void apply_mode(int mode) {
    set_hidden(mn_root, mode != MENU_LAYER);
    set_bar_hidden(mode != 0);
    if (mode == MODE_IDLE) {
        set_hidden(bt_box, false);
    }
    set_hidden(kb_icon, mode != 0);

    bool calc = (mode == CALC_LAYER);
    set_hidden(calc_expr, !calc);
    set_hidden(calc_res, !calc);
    if (!calc) {
        cm_update(false, 0);
    }

    for (int i = 0; i < 16; i++) {
        set_hidden(pg_cell[i], mode != PROG_LAYER);
    }

    invalidate_status_cache();
    menu_force = true;
}

static void refresh_cb(lv_timer_t *timer) {
    int layer = zmk_keymap_highest_layer_active();
    bool idle = (zmk_activity_get_state() != ZMK_ACTIVITY_ACTIVE);
    int mode = idle ? MODE_IDLE : layer;

    if (mode != last_mode) {
        last_mode = mode;
        apply_mode(mode);
        if (timer != NULL) {
            /* poll less often while idle */
            lv_timer_set_period(timer, idle ? 250 : 100);
        }
    }

    if (mode == MODE_IDLE) {
        uint8_t level = zmk_battery_state_of_charge();
        bool charging = zmk_usb_is_powered();
        if ((int)level != last_level || (int)charging != last_charging) {
            batt_draw(level, charging);
            last_level = level;
            last_charging = charging;
        }
        return;
    }

    if (mode == MENU_LAYER) {
        menu_refresh();
        return;
    }

    if (mode == PROG_LAYER) {
        uint32_t pv = prog_version();
        if (pv != last_prog_ver) {
            pg_update();
            last_prog_ver = pv;
        }
        return;
    }

    if (mode == CALC_LAYER) {
        calc_get_view(&cv);
        if (calc_force || strcmp(cv.disp, last_cv.disp) != 0 || strcmp(cv.res, last_cv.res) != 0 ||
            cv.cursor != last_cv.cursor || cv.menu_open != last_cv.menu_open ||
            cv.menu_sel != last_cv.menu_sel) {
            calc_view_apply(&cv);
            last_cv = cv;
            calc_force = false;
        }
        return;
    }

    /* layer 0 */
    uint8_t level = zmk_battery_state_of_charge();
    bool charging = zmk_usb_is_powered();
    bool usb = (zmk_endpoints_selected().transport == ZMK_TRANSPORT_USB);

    if ((int)level != last_level) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%u", level);
        lv_label_set_text(batt_pct, buf);
    }
    if ((int)level != last_level || (int)charging != last_charging) {
        batt_draw(level, charging);
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
}

lv_obj_t *zmk_display_status_screen() {
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    /* right bar: connection on top, percent under it, battery below */
    conn_icon = make_label(screen, &lv_font_montserrat_14, 0);
    batt_pct = make_label(screen, &lv_font_montserrat_10, 15);
    build_battery(screen);

    /* layer 0: keyboard icon in the middle of the free area */
    kb_icon = lv_label_create(screen);
    lv_obj_set_width(kb_icon, SCR_W - BAR_W);
    lv_obj_set_style_text_font(kb_icon, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_align(kb_icon, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(kb_icon, LV_SYMBOL_KEYBOARD);
    lv_obj_set_pos(kb_icon, 0, (SCR_H - lv_font_get_line_height(&lv_font_montserrat_28)) / 2);

    calc_expr = lv_label_create(screen);
    lv_obj_set_width(calc_expr, EXPR_W + 2);
    lv_label_set_long_mode(calc_expr, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(calc_expr, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_label_set_text(calc_expr, "");
    lv_obj_set_pos(calc_expr, EXPR_X, 0);

    calc_res = lv_label_create(screen);
    lv_obj_set_width(calc_res, RES_W);
    lv_label_set_long_mode(calc_res, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(calc_res, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_set_style_text_font(calc_res, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_label_set_text(calc_res, "");
    lv_obj_set_pos(calc_res, 0, SCR_H - 6 - 20);

    build_menu(screen);
    build_calc_menu(screen);
    build_prog_grid(screen);

    refresh_cb(NULL);
    lv_timer_create(refresh_cb, 100, NULL);

    return screen;
}

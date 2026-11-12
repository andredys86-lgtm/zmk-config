#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Marks a place where the expression may be wrapped (after a binary operator). */
#define CALC_MARK '\001'
#define CALC_DISP_SZ 400

struct calc_view {
    char disp[CALC_DISP_SZ]; /* expression text with CALC_MARK wrap points */
    char res[32];            /* result line: value, "=value", "-", FULL or ERROR */
    bool cursor;             /* draw the cursor now */
};

/* Key codes: 0-9 digits, 10 dot, 11 +, 12 -, 13 *, 14 /, 15 =, 16 AC,
 * 20 sqrt, 21 power, 22 pi, 23 percent, 24 +/-, 25 "(", 26 ")", 27 backspace. */
void calc_key(uint8_t key);
uint32_t calc_version(void);
void calc_get_view(struct calc_view *v);

/* Implemented by the status screen (it knows the fonts). */
bool calc_layout_fits(const char *disp);
bool calc_result_fits(const char *res);

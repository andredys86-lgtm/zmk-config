#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define PROG_KEYS 51
/* three programmable layers (keymap layers 2, 5 and 6), PROG_PAGE key slots each */
#define PROG_PAGE 17
#define PROG_LAYERS 3
#define PROG_SEQ_MAX 16
#define PROG_MENU_MAX_CELLS 24

/* 0, 1, 2 for keymap layers 2, 5, 6; -1 for any other layer */
static inline int prog_layer_index(int layer) {
    return layer == 2 ? 0 : (layer == 5 ? 1 : (layer == 6 ? 2 : -1));
}

/* A sequence element is one 32-bit word:
 * key:   (usage_page << 16) | usage_id, modifiers in the top byte
 * delay: usage page 0xFF, milliseconds in the low 16 bits */
#define PROG_DELAY_PAGE 0xFFu
#define PROG_DELAY(ms) ((PROG_DELAY_PAGE << 16) | ((uint32_t)(ms) & 0xFFFFu))
#define PROG_IS_DELAY(e) ((((e) >> 16) & 0xFFu) == PROG_DELAY_PAGE)
#define PROG_DELAY_MS(e) ((e) & 0xFFFFu)

/* menu modes */
#define PROG_MODE_PICK 0
#define PROG_MODE_GROUPS 1
#define PROG_MODE_KEYS 2
#define PROG_MODE_MODS 3
#define PROG_MODE_DELAY 4

/* centre messages */
#define PROG_MSG_NONE 0
#define PROG_MSG_SAVED 1
#define PROG_MSG_CANCELLED 2
#define PROG_MSG_FULL 3

struct prog_menu_view {
    uint8_t mode;
    uint8_t cols;
    uint8_t rows;
    uint8_t font; /* 10 or 14 */
    uint8_t cells;
    uint8_t cursor; /* in PICK mode: index of the selected key 0..15 */
    uint8_t msg_kind;
    uint8_t base; /* first key slot of the layer being edited */
    uint32_t version;
    uint32_t hl_mask; /* cells drawn with a thick border (active modifiers, delay choice) */
    char cell[PROG_MENU_MAX_CELLS][8];
    char seq[200];
};

void prog_play(uint32_t idx);
void prog_menu_event(uint32_t ev);
void prog_get_menu_view(struct prog_menu_view *v);
uint32_t prog_version(void);

/* Text of the saved sequence of key idx (0..15); empty string when nothing is assigned. */
void prog_key_text(uint32_t idx, char *out, size_t n);

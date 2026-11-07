#pragma once

#include <stdint.h>
#include <stdbool.h>

#define PROG_KEYS 17
#define PROG_SEQ_MAX 16

/* A sequence element is one 32-bit word:
 * key:   (usage_page << 16) | usage_id, modifiers in the top byte
 * delay: usage page 0xFF, milliseconds in the low 16 bits */
#define PROG_DELAY_PAGE 0xFFu
#define PROG_DELAY(ms) ((PROG_DELAY_PAGE << 16) | ((uint32_t)(ms) & 0xFFFFu))
#define PROG_IS_DELAY(e) ((((e) >> 16) & 0xFFu) == PROG_DELAY_PAGE)
#define PROG_DELAY_MS(e) ((e) & 0xFFFFu)

struct prog_view {
    char title[24];
    char rows[3][24];
};

void prog_play(uint32_t idx);
void prog_menu_event(uint32_t ev);
void prog_get_view(struct prog_view *v);
uint32_t prog_version(void);

#pragma once

#include <stdint.h>
#include <stdbool.h>

#define PROG_KEYS 17

struct prog_view {
    uint8_t state;
    int target;
    const char *name;
    bool saved;
};

uint32_t prog_get_code(uint32_t idx);
void prog_menu_event(uint32_t ev);
void prog_get_view(struct prog_view *v);
uint32_t prog_version(void);

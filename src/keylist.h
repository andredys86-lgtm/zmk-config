#pragma once

#include <stdint.h>
#include <stddef.h>

enum key_group {
    G_LETTERS,
    G_DIGITS,
    G_FKEYS,
    G_NAV,
    G_SYMBOLS,
    G_KEYPAD,
    G_MEDIA,
    G_COUNT
};

struct key_entry {
    const char *name;
    uint32_t code;
    uint8_t group;
};

extern const struct key_entry key_list[];
extern const size_t key_list_len;
extern const char *const group_names[G_COUNT];

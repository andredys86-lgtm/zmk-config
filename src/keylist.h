#pragma once

#include <stdint.h>
#include <stddef.h>

/* key groups: values 0..G_COUNT-1 are also the first cells of the group grid */
enum key_group {
    G_ABC,
    G_NUM,
    G_FKEYS,
    G_NAV,
    G_SYM,
    G_KP,
    G_MED,
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
extern const uint8_t group_cols[G_COUNT];
extern const uint8_t group_rows[G_COUNT];
extern const uint8_t group_font[G_COUNT];

const char *key_label(uint32_t code);

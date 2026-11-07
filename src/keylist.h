#pragma once

#include <stdint.h>
#include <stddef.h>

struct key_entry {
    const char *name;
    uint32_t code;
};

extern const struct key_entry key_list[];
extern const size_t key_list_len;

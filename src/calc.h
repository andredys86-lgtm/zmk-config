#pragma once

#include <stdint.h>
#include <stddef.h>

void calc_key(uint8_t key);
uint32_t calc_version(void);
void calc_get_display(char *num, size_t num_len, char *op);

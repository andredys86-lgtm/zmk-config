#include "keylist.h"

#define KB(id) (0x070000u | (id))
#define CONS(id) (0x0C0000u | (id))

const struct key_entry key_list[] = {
    {"None", 0},
    {"A", KB(0x04)}, {"B", KB(0x05)}, {"C", KB(0x06)}, {"D", KB(0x07)},
    {"E", KB(0x08)}, {"F", KB(0x09)}, {"G", KB(0x0A)}, {"H", KB(0x0B)},
    {"I", KB(0x0C)}, {"J", KB(0x0D)}, {"K", KB(0x0E)}, {"L", KB(0x0F)},
    {"M", KB(0x10)}, {"N", KB(0x11)}, {"O", KB(0x12)}, {"P", KB(0x13)},
    {"Q", KB(0x14)}, {"R", KB(0x15)}, {"S", KB(0x16)}, {"T", KB(0x17)},
    {"U", KB(0x18)}, {"V", KB(0x19)}, {"W", KB(0x1A)}, {"X", KB(0x1B)},
    {"Y", KB(0x1C)}, {"Z", KB(0x1D)},
    {"1", KB(0x1E)}, {"2", KB(0x1F)}, {"3", KB(0x20)}, {"4", KB(0x21)},
    {"5", KB(0x22)}, {"6", KB(0x23)}, {"7", KB(0x24)}, {"8", KB(0x25)},
    {"9", KB(0x26)}, {"0", KB(0x27)},
    {"Enter", KB(0x28)}, {"Esc", KB(0x29)}, {"Bksp", KB(0x2A)}, {"Tab", KB(0x2B)},
    {"Space", KB(0x2C)}, {"-", KB(0x2D)}, {"=", KB(0x2E)}, {"[", KB(0x2F)},
    {"]", KB(0x30)}, {"\\", KB(0x31)}, {";", KB(0x33)}, {"'", KB(0x34)},
    {"`", KB(0x35)}, {",", KB(0x36)}, {".", KB(0x37)}, {"/", KB(0x38)},
    {"CapsLk", KB(0x39)},
    {"F1", KB(0x3A)}, {"F2", KB(0x3B)}, {"F3", KB(0x3C)}, {"F4", KB(0x3D)},
    {"F5", KB(0x3E)}, {"F6", KB(0x3F)}, {"F7", KB(0x40)}, {"F8", KB(0x41)},
    {"F9", KB(0x42)}, {"F10", KB(0x43)}, {"F11", KB(0x44)}, {"F12", KB(0x45)},
    {"PrtScr", KB(0x46)}, {"ScrLk", KB(0x47)}, {"Pause", KB(0x48)},
    {"Ins", KB(0x49)}, {"Home", KB(0x4A)}, {"PgUp", KB(0x4B)}, {"Del", KB(0x4C)},
    {"End", KB(0x4D)}, {"PgDn", KB(0x4E)},
    {"Right", KB(0x4F)}, {"Left", KB(0x50)}, {"Down", KB(0x51)}, {"Up", KB(0x52)},
    {"NumLk", KB(0x53)},
    {"KP /", KB(0x54)}, {"KP *", KB(0x55)}, {"KP -", KB(0x56)}, {"KP +", KB(0x57)},
    {"KP Ent", KB(0x58)},
    {"KP 1", KB(0x59)}, {"KP 2", KB(0x5A)}, {"KP 3", KB(0x5B)}, {"KP 4", KB(0x5C)},
    {"KP 5", KB(0x5D)}, {"KP 6", KB(0x5E)}, {"KP 7", KB(0x5F)}, {"KP 8", KB(0x60)},
    {"KP 9", KB(0x61)}, {"KP 0", KB(0x62)}, {"KP .", KB(0x63)},
    {"Vol +", CONS(0xE9)}, {"Vol -", CONS(0xEA)}, {"Mute", CONS(0xE2)},
    {"Play", CONS(0xCD)}, {"Next", CONS(0xB5)}, {"Prev", CONS(0xB6)},
};

const size_t key_list_len = sizeof(key_list) / sizeof(key_list[0]);

#include "keylist.h"

#define KB(id) (0x070000u | (id))
#define SH(id) (0x070000u | (id) | (0x02u << 24))

#define ABC(n, id) {n, KB(id), G_ABC}
#define NUM(n, id) {n, KB(id), G_NUM}
#define FN(n, id) {n, KB(id), G_FKEYS}
#define NAV(n, id) {n, KB(id), G_NAV}
#define SYM(n, id) {n, KB(id), G_SYM}
#define SHS(n, id) {n, SH(id), G_SYM}
#define KPD(n, id) {n, KB(id), G_KP}

const char *const group_names[G_COUNT] = {
    "ABC", "NUM", "F-F", "NAV", "SYM", "KP",
};

/* columns of the key page grid for every group */
const uint8_t group_cols[G_COUNT] = {6, 6, 4, 3, 6, 3};

const struct key_entry key_list[] = {
    ABC("A", 0x04), ABC("B", 0x05), ABC("C", 0x06), ABC("D", 0x07), ABC("E", 0x08),
    ABC("F", 0x09), ABC("G", 0x0A), ABC("H", 0x0B), ABC("I", 0x0C), ABC("J", 0x0D),
    ABC("K", 0x0E), ABC("L", 0x0F), ABC("M", 0x10), ABC("N", 0x11), ABC("O", 0x12),
    ABC("P", 0x13), ABC("Q", 0x14), ABC("R", 0x15), ABC("S", 0x16), ABC("T", 0x17),
    ABC("U", 0x18), ABC("V", 0x19), ABC("W", 0x1A), ABC("X", 0x1B), ABC("Y", 0x1C),
    ABC("Z", 0x1D),

    NUM("0", 0x27), NUM("1", 0x1E), NUM("2", 0x1F), NUM("3", 0x20), NUM("4", 0x21),
    NUM("5", 0x22), NUM("6", 0x23), NUM("7", 0x24), NUM("8", 0x25), NUM("9", 0x26),

    FN("F1", 0x3A), FN("F2", 0x3B), FN("F3", 0x3C), FN("F4", 0x3D),
    FN("F5", 0x3E), FN("F6", 0x3F), FN("F7", 0x40), FN("F8", 0x41),
    FN("F9", 0x42), FN("F10", 0x43), FN("F11", 0x44), FN("F12", 0x45),

    NAV("<", 0x50), NAV(">", 0x4F), NAV("^", 0x52), NAV("v", 0x51),
    NAV("HOME", 0x4A), NAV("END", 0x4D), NAV("PGUP", 0x4B), NAV("PGDN", 0x4E),
    NAV("INS", 0x49), NAV("DEL", 0x4C), NAV("BS", 0x2A), NAV("ENT", 0x28),
    NAV("TAB", 0x2B), NAV("ESC", 0x29), NAV("SPC", 0x2C), NAV("CAPS", 0x39),

    SHS("!", 0x1E), SHS("@", 0x1F), SHS("#", 0x20), SHS("$", 0x21), SHS("%", 0x22),
    SHS("^", 0x23), SHS("&", 0x24), SHS("*", 0x25), SHS("(", 0x26), SHS(")", 0x27),
    SYM("-", 0x2D), SHS("_", 0x2D), SYM("=", 0x2E), SHS("+", 0x2E), SYM("[", 0x2F),
    SYM("]", 0x30), SHS("{", 0x2F), SHS("}", 0x30), SYM("\\", 0x31), SHS("|", 0x31),
    SYM(";", 0x33), SHS(":", 0x33), SYM("'", 0x34), SHS("\"", 0x34), SYM(",", 0x36),
    SYM(".", 0x37), SHS("<", 0x36), SHS(">", 0x37), SYM("/", 0x38), SHS("?", 0x38),
    SHS("~", 0x35), SYM("`", 0x35),

    KPD("KP0", 0x62), KPD("KP1", 0x59), KPD("KP2", 0x5A), KPD("KP3", 0x5B),
    KPD("KP4", 0x5C), KPD("KP5", 0x5D), KPD("KP6", 0x5E), KPD("KP7", 0x5F),
    KPD("KP8", 0x60), KPD("KP9", 0x61), KPD("KP.", 0x63), KPD("KPEN", 0x58),
    KPD("KP+", 0x57), KPD("KP-", 0x56), KPD("KP*", 0x55), KPD("KP/", 0x54),
};

const size_t key_list_len = sizeof(key_list) / sizeof(key_list[0]);

const char *key_label(uint32_t code) {
    for (size_t i = 0; i < key_list_len; i++) {
        if (key_list[i].code == code) {
            return key_list[i].name;
        }
    }
    return NULL;
}

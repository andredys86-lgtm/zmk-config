#include "keylist.h"

#define KB(id) (0x070000u | (id))
#define CONS(id) (0x0C0000u | (id))

#define LET(n, id) {n, KB(id), G_LETTERS}
#define DIG(n, id) {n, KB(id), G_DIGITS}
#define FN(n, id) {n, KB(id), G_FKEYS}
#define NAV(n, id) {n, KB(id), G_NAV}
#define SYM(n, id) {n, KB(id), G_SYMBOLS}
#define KPD(n, id) {n, KB(id), G_KEYPAD}
#define MED(n, id) {n, CONS(id), G_MEDIA}

const char *const group_names[G_COUNT] = {
    "Letters", "Digits", "F1-F12", "Navigation", "Symbols", "Keypad", "Media",
};

const struct key_entry key_list[] = {
    LET("A", 0x04), LET("B", 0x05), LET("C", 0x06), LET("D", 0x07), LET("E", 0x08),
    LET("F", 0x09), LET("G", 0x0A), LET("H", 0x0B), LET("I", 0x0C), LET("J", 0x0D),
    LET("K", 0x0E), LET("L", 0x0F), LET("M", 0x10), LET("N", 0x11), LET("O", 0x12),
    LET("P", 0x13), LET("Q", 0x14), LET("R", 0x15), LET("S", 0x16), LET("T", 0x17),
    LET("U", 0x18), LET("V", 0x19), LET("W", 0x1A), LET("X", 0x1B), LET("Y", 0x1C),
    LET("Z", 0x1D),

    DIG("1", 0x1E), DIG("2", 0x1F), DIG("3", 0x20), DIG("4", 0x21), DIG("5", 0x22),
    DIG("6", 0x23), DIG("7", 0x24), DIG("8", 0x25), DIG("9", 0x26), DIG("0", 0x27),

    FN("F1", 0x3A), FN("F2", 0x3B), FN("F3", 0x3C), FN("F4", 0x3D),
    FN("F5", 0x3E), FN("F6", 0x3F), FN("F7", 0x40), FN("F8", 0x41),
    FN("F9", 0x42), FN("F10", 0x43), FN("F11", 0x44), FN("F12", 0x45),

    NAV("Enter", 0x28), NAV("Esc", 0x29), NAV("Backspace", 0x2A), NAV("Tab", 0x2B),
    NAV("Space", 0x2C), NAV("CapsLock", 0x39), NAV("PrtScr", 0x46), NAV("ScrLock", 0x47),
    NAV("Pause", 0x48), NAV("Insert", 0x49), NAV("Home", 0x4A), NAV("PgUp", 0x4B),
    NAV("Delete", 0x4C), NAV("End", 0x4D), NAV("PgDn", 0x4E), NAV("Right", 0x4F),
    NAV("Left", 0x50), NAV("Down", 0x51), NAV("Up", 0x52),

    SYM("-", 0x2D), SYM("=", 0x2E), SYM("[", 0x2F), SYM("]", 0x30), SYM("\\", 0x31),
    SYM(";", 0x33), SYM("'", 0x34), SYM("`", 0x35), SYM(",", 0x36), SYM(".", 0x37),
    SYM("/", 0x38),

    KPD("NumLock", 0x53), KPD("KP /", 0x54), KPD("KP *", 0x55), KPD("KP -", 0x56),
    KPD("KP +", 0x57), KPD("KP Enter", 0x58), KPD("KP 1", 0x59), KPD("KP 2", 0x5A),
    KPD("KP 3", 0x5B), KPD("KP 4", 0x5C), KPD("KP 5", 0x5D), KPD("KP 6", 0x5E),
    KPD("KP 7", 0x5F), KPD("KP 8", 0x60), KPD("KP 9", 0x61), KPD("KP 0", 0x62),
    KPD("KP .", 0x63),

    MED("Vol +", 0xE9), MED("Vol -", 0xEA), MED("Mute", 0xE2),
    MED("Play/Pause", 0xCD), MED("Next", 0xB5), MED("Prev", 0xB6),
};

const size_t key_list_len = sizeof(key_list) / sizeof(key_list[0]);

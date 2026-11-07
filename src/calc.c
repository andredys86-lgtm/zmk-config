#include <zephyr/kernel.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "calc.h"

#define KEY_DOT 10
#define KEY_ADD 11
#define KEY_SUB 12
#define KEY_MUL 13
#define KEY_DIV 14
#define KEY_EQ 15
#define KEY_CLR 16

#define MAX_DIGITS 8
#define LIMIT 100000000.0

static K_MUTEX_DEFINE(calc_lock);

static double acc;
static bool have_acc;
static char entry[16];
static int entry_len;
static int entry_digits;
static bool entry_dot;
static bool entering;
static bool just_eval;
static bool error;
static char pending_op;
static uint32_t version;

static void reset_all(void) {
    acc = 0;
    have_acc = false;
    entry[0] = '\0';
    entry_len = 0;
    entry_digits = 0;
    entry_dot = false;
    entering = false;
    just_eval = false;
    error = false;
    pending_op = 0;
}

static double parse_entry(void) {
    double v = 0;
    double scale = 0.1;
    bool frac = false;
    for (int i = 0; i < entry_len; i++) {
        char c = entry[i];
        if (c == '.') {
            frac = true;
            continue;
        }
        int d = c - '0';
        if (!frac) {
            v = v * 10 + d;
        } else {
            v += d * scale;
            scale /= 10;
        }
    }
    return v;
}

static bool apply(char op, double a, double b, double *out) {
    double r;
    switch (op) {
    case '+':
        r = a + b;
        break;
    case '-':
        r = a - b;
        break;
    case '*':
        r = a * b;
        break;
    case '/':
        if (b == 0) {
            return false;
        }
        r = a / b;
        break;
    default:
        r = b;
        break;
    }
    if (r >= LIMIT || r <= -LIMIT) {
        return false;
    }
    *out = r;
    return true;
}

static void begin_entry(void) {
    if (just_eval) {
        have_acc = false;
        pending_op = 0;
        just_eval = false;
    }
    entry[0] = '\0';
    entry_len = 0;
    entry_digits = 0;
    entry_dot = false;
    entering = true;
}

static void press_digit(int d) {
    if (!entering) {
        begin_entry();
    }
    if (entry_digits >= MAX_DIGITS) {
        return;
    }
    if (entry_len == 1 && entry[0] == '0' && !entry_dot) {
        entry_len = 0;
        entry_digits = 0;
    }
    entry[entry_len++] = (char)('0' + d);
    entry[entry_len] = '\0';
    entry_digits++;
}

static void press_dot(void) {
    if (!entering) {
        begin_entry();
    }
    if (entry_dot) {
        return;
    }
    if (entry_len == 0) {
        entry[0] = '0';
        entry[1] = '\0';
        entry_len = 1;
        entry_digits = 1;
    }
    if (entry_digits >= MAX_DIGITS) {
        return;
    }
    entry[entry_len++] = '.';
    entry[entry_len] = '\0';
    entry_dot = true;
}

static void press_op(char o) {
    if (entering) {
        double v = parse_entry();
        if (pending_op && have_acc) {
            double r;
            if (!apply(pending_op, acc, v, &r)) {
                error = true;
                return;
            }
            acc = r;
        } else {
            acc = v;
        }
        have_acc = true;
        entering = false;
    } else if (!have_acc) {
        acc = 0;
        have_acc = true;
    }
    just_eval = false;
    pending_op = o;
}

static void press_eq(void) {
    double r;
    if (entering) {
        double v = parse_entry();
        if (pending_op && have_acc) {
            if (!apply(pending_op, acc, v, &r)) {
                error = true;
                return;
            }
            acc = r;
        } else {
            acc = v;
        }
        have_acc = true;
        entering = false;
        pending_op = 0;
        just_eval = true;
    } else if (pending_op && have_acc) {
        if (!apply(pending_op, acc, acc, &r)) {
            error = true;
            return;
        }
        acc = r;
        pending_op = 0;
        just_eval = true;
    }
}

static void format_value(double x, char *out, size_t n) {
    bool neg = x < 0;
    if (neg) {
        x = -x;
    }
    int64_t scaled = (int64_t)(x * 1000000.0 + 0.5);
    int ip = (int)(scaled / 1000000);
    int fp = (int)(scaled % 1000000);
    char buf[24];
    int len = 0;
    if (neg && (ip != 0 || fp != 0)) {
        buf[len++] = '-';
    }
    len += snprintf(buf + len, sizeof(buf) - len, "%d", ip);
    if (fp != 0) {
        len += snprintf(buf + len, sizeof(buf) - len, ".%06d", fp);
        while (buf[len - 1] == '0') {
            buf[--len] = '\0';
        }
    }
    if (len > 9) {
        len = 9;
        buf[len] = '\0';
        if (buf[len - 1] == '.') {
            buf[--len] = '\0';
        }
    }
    snprintf(out, n, "%s", buf);
}

void calc_key(uint8_t k) {
    k_mutex_lock(&calc_lock, K_FOREVER);
    if (k == KEY_CLR) {
        reset_all();
    } else {
        if (error) {
            reset_all();
        }
        if (k <= 9) {
            press_digit(k);
        } else if (k == KEY_DOT) {
            press_dot();
        } else if (k == KEY_ADD) {
            press_op('+');
        } else if (k == KEY_SUB) {
            press_op('-');
        } else if (k == KEY_MUL) {
            press_op('*');
        } else if (k == KEY_DIV) {
            press_op('/');
        } else if (k == KEY_EQ) {
            press_eq();
        }
    }
    version++;
    k_mutex_unlock(&calc_lock);
}

uint32_t calc_version(void) {
    return version;
}

void calc_get_display(char *num, size_t num_len, char *op) {
    k_mutex_lock(&calc_lock, K_FOREVER);
    if (error) {
        snprintf(num, num_len, "Err");
        *op = 0;
    } else {
        if (entering) {
            snprintf(num, num_len, "%s", entry);
        } else if (have_acc) {
            format_value(acc, num, num_len);
        } else {
            snprintf(num, num_len, "0");
        }
        *op = pending_op;
    }
    k_mutex_unlock(&calc_lock);
}

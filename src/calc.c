#include <zephyr/kernel.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "calc.h"

#define KEY_DOT 10
#define KEY_ADD 11
#define KEY_SUB 12
#define KEY_MUL 13
#define KEY_DIV 14
#define KEY_EQ 15
#define KEY_CLR 16
#define KEY_SQRT 20
#define KEY_POW 21
#define KEY_PI 22
#define KEY_PCT 23
#define KEY_SIGN 24
#define KEY_OPEN 25
#define KEY_CLOSE 26
#define KEY_BS 27

#define CAP 96
#define MAX_PAREN 2
#define MAX_POW 2
#define PI_VAL 3.14159265358979323846

/*
 * The expression is a string of atoms:
 *  digits, '.', '+', '-', '*', '/' (binary operators),
 *  'N' unary minus, 'P' pi, '%' percent,
 *  '(' ')' ordinary parentheses,
 *  'R' "sqrt(" and 'r' its closing, 'W' "^(" and 'w' its closing.
 */

static K_MUTEX_DEFINE(calc_lock);

static char buf[CAP];
static int len;
static bool done;
static bool full;
static bool err;
static double res_val;
static char res_txt[32];
static char interm[32] = "0";
static char disp_scratch[CALC_DISP_SZ];
static uint32_t version;
static uint32_t last_input;

__weak bool calc_layout_fits(const char *disp) {
    (void)disp;
    return true;
}

__weak bool calc_result_fits(const char *res) {
    (void)res;
    return true;
}

enum cat { C_NONE, C_NUM, C_PI, C_PCT, C_OP, C_NEG, C_OPEN, C_CP, C_CR, C_CW };

struct ctx {
    int np;
    int nw;
    bool in_root;
    char top;
};

static bool is_digit_atom(char c) {
    return (c >= '0' && c <= '9') || c == '.';
}

static bool is_op(char c) {
    return c == '+' || c == '-' || c == '*' || c == '/';
}

static enum cat cat_of(void) {
    if (len == 0) {
        return C_NONE;
    }
    char c = buf[len - 1];
    if (is_digit_atom(c)) {
        return C_NUM;
    }
    switch (c) {
    case 'P':
        return C_PI;
    case '%':
        return C_PCT;
    case 'N':
        return C_NEG;
    case '(':
    case 'R':
    case 'W':
        return C_OPEN;
    case ')':
        return C_CP;
    case 'r':
        return C_CR;
    case 'w':
        return C_CW;
    default:
        return C_OP;
    }
}

static void scan(struct ctx *c) {
    char st[CAP];
    int sp = 0;
    for (int i = 0; i < len; i++) {
        char ch = buf[i];
        if (ch == '(' || ch == 'R' || ch == 'W') {
            if (sp < CAP) {
                st[sp++] = ch;
            }
        } else if ((ch == ')' || ch == 'r' || ch == 'w') && sp > 0) {
            sp--;
        }
    }
    c->np = 0;
    c->nw = 0;
    c->in_root = false;
    for (int j = 0; j < sp; j++) {
        if (st[j] == '(') {
            c->np++;
        } else if (st[j] == 'W') {
            c->nw++;
        } else if (st[j] == 'R') {
            c->in_root = true;
        }
    }
    c->top = sp > 0 ? st[sp - 1] : 0;
}

static bool put(char c) {
    if (len >= CAP - 1) {
        return false;
    }
    buf[len++] = c;
    return true;
}

static void insert_at(int pos, char c) {
    if (len >= CAP - 1) {
        return;
    }
    memmove(buf + pos + 1, buf + pos, len - pos);
    buf[pos] = c;
    len++;
}

static void remove_at(int pos) {
    memmove(buf + pos, buf + pos + 1, len - pos - 1);
    len--;
}

static void strip_dot(void) {
    if (len > 0 && buf[len - 1] == '.') {
        len--;
    }
}

static int run_start(int n) {
    int i = n;
    while (i > 0 && is_digit_atom(buf[i - 1])) {
        i--;
    }
    return i;
}

/* Start of the literal (number or pi, with optional % and unary minus) that ends at n. */
static int lit_start(int n) {
    int e = n;
    int s;
    if (e > 0 && buf[e - 1] == '%') {
        e--;
    }
    if (e > 0 && buf[e - 1] == 'P') {
        s = e - 1;
    } else {
        s = run_start(e);
    }
    if (s > 0 && buf[s - 1] == 'N') {
        s--;
    }
    return s;
}

static void build_disp(const char *a, int n, char *out, size_t sz) {
    size_t o = 0;
    for (int i = 0; i < n; i++) {
        char c = a[i];
        char tmp[2];
        const char *s;
        switch (c) {
        case 'N':
            s = "-";
            break;
        case 'P':
            s = "PI";
            break;
        case 'R':
            s = "SQ(";
            break;
        case 'W':
            s = "^(";
            break;
        case 'r':
        case 'w':
            s = ")";
            break;
        default:
            tmp[0] = c;
            tmp[1] = '\0';
            s = tmp;
            break;
        }
        size_t l = strlen(s);
        if (o + l + 2 >= sz) {
            break;
        }
        memcpy(out + o, s, l);
        o += l;
        if (is_op(c)) {
            out[o++] = CALC_MARK;
        }
    }
    out[o] = '\0';
}

/* ---------- evaluation ---------- */

#define P_OK 0
#define P_INC 1
#define P_SYN 2
#define P_MATH 3

static const char *ea;
static int en;
static int ep;
static int perr;
static bool pin_root;
static bool pin_pow;
static bool g_pct;
static bool g_tpct;

static bool bad(double x) {
    return !(x == x) || x > 1.0e15 || x < -1.0e15;
}

static double parse_expr(void);

static double parse_primary(void) {
    double v = 0;
    bool neg = false;
    bool pct = false;
    bool lit = false;
    char c;

    g_pct = false;
    if (ep >= en) {
        perr = P_INC;
        return 0;
    }
    c = ea[ep];
    if (c == 'N') {
        neg = true;
        ep++;
        if (ep >= en) {
            perr = P_INC;
            return 0;
        }
        c = ea[ep];
    }

    if (c == '(') {
        if (neg) {
            perr = P_SYN;
            return 0;
        }
        ep++;
        if (ep < en && ea[ep] == ')') {
            perr = P_SYN;
            return 0;
        }
        v = parse_expr();
        if (perr) {
            return 0;
        }
        if (ep < en && ea[ep] == ')') {
            ep++;
        } else {
            perr = (ep >= en) ? P_INC : P_SYN;
            return 0;
        }
    } else if (c == 'R') {
        if (neg || pin_root) {
            perr = P_SYN;
            return 0;
        }
        ep++;
        pin_root = true;
        v = parse_expr();
        pin_root = false;
        if (perr) {
            return 0;
        }
        if (ep < en && ea[ep] == 'r') {
            ep++;
        } else {
            perr = (ep >= en) ? P_INC : P_SYN;
            return 0;
        }
        if (v < 0) {
            perr = P_MATH;
            return 0;
        }
        v = sqrt(v);
    } else if (is_digit_atom(c)) {
        double x = 0;
        double sc = 0.1;
        bool frac = false;
        while (ep < en && is_digit_atom(ea[ep])) {
            char d = ea[ep++];
            if (d == '.') {
                frac = true;
                continue;
            }
            if (!frac) {
                x = x * 10 + (d - '0');
            } else {
                x += (d - '0') * sc;
                sc /= 10;
            }
        }
        v = x;
        lit = true;
    } else if (c == 'P') {
        ep++;
        v = PI_VAL;
        lit = true;
    } else {
        perr = P_SYN;
        return 0;
    }

    if (lit && ep < en && ea[ep] == '%') {
        if (pin_pow) {
            perr = P_SYN;
            return 0;
        }
        ep++;
        v /= 100.0;
        pct = true;
    }
    if (neg) {
        v = -v;
    }

    if (ep < en && ea[ep] == 'W') {
        if (pct) {
            perr = P_SYN;
            return 0;
        }
        ep++;
        int st = ep;
        bool saved = pin_pow;
        pin_pow = true;
        double e = parse_expr();
        pin_pow = saved;
        if (perr) {
            return 0;
        }
        int ed = ep;
        if (ep < en && ea[ep] == 'w') {
            ep++;
        } else {
            perr = (ep >= en) ? P_INC : P_SYN;
            return 0;
        }
        bool simple = true;
        for (int i = st; i < ed; i++) {
            if (!is_digit_atom(ea[i])) {
                simple = false;
            }
        }
        if (simple && e == 0) {
            perr = P_SYN;
            return 0;
        }
        if (v <= 0) {
            perr = P_MATH;
            return 0;
        }
        double r = pow(v, e);
        if (bad(r)) {
            perr = P_MATH;
            return 0;
        }
        v = r;
        pct = false;
    }

    g_pct = pct;
    return v;
}

static double parse_term(void) {
    double v = parse_primary();
    if (perr) {
        return 0;
    }
    bool single = g_pct;
    while (ep < en && (ea[ep] == '*' || ea[ep] == '/')) {
        char o = ea[ep++];
        double r = parse_primary();
        if (perr) {
            return 0;
        }
        single = false;
        if (o == '*') {
            v *= r;
        } else {
            if (r == 0) {
                perr = P_MATH;
                return 0;
            }
            v /= r;
        }
        if (bad(v)) {
            perr = P_MATH;
            return 0;
        }
    }
    g_tpct = single;
    return v;
}

static double parse_expr(void) {
    double v = parse_term();
    if (perr) {
        return 0;
    }
    while (ep < en && (ea[ep] == '+' || ea[ep] == '-')) {
        char o = ea[ep++];
        double r = parse_term();
        if (perr) {
            return 0;
        }
        if (g_tpct) {
            r = v * r;
        }
        v = (o == '+') ? v + r : v - r;
        if (bad(v)) {
            perr = P_MATH;
            return 0;
        }
    }
    return v;
}

static int evaluate(const char *a, int n, double *out) {
    ea = a;
    en = n;
    ep = 0;
    perr = P_OK;
    pin_root = false;
    pin_pow = false;
    if (n == 0) {
        return P_INC;
    }
    double v = parse_expr();
    if (!perr && ep < en) {
        perr = P_SYN;
    }
    if (perr) {
        return perr;
    }
    *out = v;
    return P_OK;
}

static bool format_value(double x, char *out, size_t n) {
    bool neg = x < 0;
    if (neg) {
        x = -x;
    }
    if (x >= 9.0e15) {
        return false;
    }
    uint64_t ip = (uint64_t)x;
    double fr = x - (double)ip;
    uint32_t fp = (uint32_t)(fr * 1000000.0 + 0.5);
    if (fp >= 1000000u) {
        ip++;
        fp -= 1000000u;
    }
    char b[32];
    int l = 0;
    if (neg && (ip != 0 || fp != 0)) {
        b[l++] = '-';
    }
    char dg[24];
    int dn = 0;
    if (ip == 0) {
        dg[dn++] = '0';
    } else {
        while (ip > 0) {
            dg[dn++] = (char)('0' + (int)(ip % 10));
            ip /= 10;
        }
    }
    while (dn > 0) {
        b[l++] = dg[--dn];
    }
    if (fp != 0) {
        char fd[6];
        for (int i = 5; i >= 0; i--) {
            fd[i] = (char)('0' + (int)(fp % 10));
            fp /= 10;
        }
        int e = 6;
        while (e > 0 && fd[e - 1] == '0') {
            e--;
        }
        b[l++] = '.';
        for (int i = 0; i < e; i++) {
            b[l++] = fd[i];
        }
    }
    b[l] = '\0';
    snprintf(out, n, "%s", b);
    return true;
}

/* Closes open sqrt / power templates (innermost first). */
static int auto_close(char *a, int n) {
    char st[CAP];
    int sp = 0;
    for (int i = 0; i < n; i++) {
        char ch = a[i];
        if (ch == '(' || ch == 'R' || ch == 'W') {
            if (sp < CAP) {
                st[sp++] = ch;
            }
        } else if ((ch == ')' || ch == 'r' || ch == 'w') && sp > 0) {
            sp--;
        }
    }
    while (sp > 0 && n < CAP + 3 && (st[sp - 1] == 'R' || st[sp - 1] == 'W')) {
        a[n++] = (st[sp - 1] == 'R') ? 'r' : 'w';
        sp--;
    }
    return n;
}

static int prep(char *tmp, bool strip) {
    int n = len;
    memcpy(tmp, buf, n);
    if (strip) {
        while (n > 0 && (is_op(tmp[n - 1]) || tmp[n - 1] == 'N')) {
            n--;
        }
    }
    return auto_close(tmp, n);
}

static bool final_calc(const char *a, int n, double *v, char *t, size_t tn) {
    if (evaluate(a, n, v) != P_OK) {
        return false;
    }
    if (!format_value(*v, t, tn)) {
        return false;
    }
    char w[40];
    snprintf(w, sizeof(w), "=%s", t);
    return calc_result_fits(w);
}

static void calc_interm(void) {
    if (len == 0) {
        snprintf(interm, sizeof(interm), "0");
        return;
    }
    char tmp[CAP + 4];
    int n = prep(tmp, false);
    double v;
    char t[32];
    if (evaluate(tmp, n, &v) == P_OK && format_value(v, t, sizeof(t)) && calc_result_fits(t)) {
        snprintf(interm, sizeof(interm), "%s", t);
    } else {
        snprintf(interm, sizeof(interm), "-");
    }
}

static void recompute(void) {
    build_disp(buf, len, disp_scratch, sizeof(disp_scratch));
    full = (len >= CAP - 6) || !calc_layout_fits(disp_scratch);
    if (err) {
        if (len == 0) {
            err = false;
        } else {
            char tmp[CAP + 4];
            int n = prep(tmp, true);
            double v;
            char t[32];
            if (final_calc(tmp, n, &v, t, sizeof(t))) {
                err = false;
            }
        }
    }
    if (done || err) {
        return;
    }
    calc_interm();
}

/* ---------- editing ---------- */

static void reset_all(void) {
    len = 0;
    done = false;
    full = false;
    err = false;
    res_val = 0;
    res_txt[0] = '\0';
}

static void new_expr(void) {
    len = 0;
    done = false;
}

static void load_value(double v) {
    char t[32];
    len = 0;
    done = false;
    if (!format_value(v, t, sizeof(t))) {
        return;
    }
    for (const char *p = t; *p; p++) {
        buf[len++] = (*p == '-') ? 'N' : *p;
    }
}

static void press_digit(int d) {
    if (done) {
        new_expr();
    }
    char ch = (char)('0' + d);
    switch (cat_of()) {
    case C_NUM: {
        int s = run_start(len);
        if (len - s == 1 && buf[s] == '0') {
            if (d != 0) {
                buf[s] = ch;
            }
            return;
        }
        put(ch);
        return;
    }
    case C_PI:
    case C_CR:
        put('*');
        put(ch);
        return;
    case C_PCT:
    case C_CP:
    case C_CW:
        return;
    default:
        put(ch);
        return;
    }
}

static void press_dot(void) {
    if (done) {
        new_expr();
        put('0');
        put('.');
        return;
    }
    switch (cat_of()) {
    case C_NUM: {
        int s = run_start(len);
        for (int i = s; i < len; i++) {
            if (buf[i] == '.') {
                return;
            }
        }
        put('.');
        return;
    }
    case C_NONE:
    case C_OP:
    case C_NEG:
    case C_OPEN:
        put('0');
        put('.');
        return;
    default:
        return;
    }
}

static void press_op(char o) {
    if (done) {
        load_value(res_val);
    }
    switch (cat_of()) {
    case C_NONE:
    case C_OPEN:
        if (o == '-') {
            put('N');
        }
        return;
    case C_NEG:
        return;
    case C_OP:
        buf[len - 1] = o;
        return;
    default:
        strip_dot();
        put(o);
        return;
    }
}

static void press_eq(void) {
    if (err || done || len == 0) {
        return;
    }
    char tmp[CAP + 4];
    int n = prep(tmp, true);
    double v;
    char t[32];
    if (!final_calc(tmp, n, &v, t, sizeof(t))) {
        err = true;
        return;
    }
    memcpy(buf, tmp, n);
    len = n;
    done = true;
    res_val = v;
    snprintf(res_txt, sizeof(res_txt), "%s", t);
}

static void press_sqrt(void) {
    if (done) {
        load_value(res_val);
        insert_at(0, 'R');
        put('r');
        return;
    }
    struct ctx x;
    scan(&x);
    switch (cat_of()) {
    case C_NUM:
        strip_dot();
        /* fall through */
    case C_PI:
    case C_PCT: {
        int s = lit_start(len);
        insert_at(s, 'R');
        put('r');
        if (x.in_root) {
            err = true;
        }
        return;
    }
    case C_NONE:
    case C_OP:
    case C_OPEN:
        put('R');
        if (x.in_root) {
            err = true;
        }
        return;
    default:
        return;
    }
}

static void press_pow(void) {
    if (done) {
        load_value(res_val);
        put('W');
        return;
    }
    struct ctx x;
    scan(&x);
    if (x.nw >= MAX_POW) {
        return;
    }
    switch (cat_of()) {
    case C_NUM:
        strip_dot();
        put('W');
        return;
    case C_PI:
    case C_CR:
        put('W');
        return;
    default:
        return;
    }
}

static void press_pi(void) {
    if (done) {
        new_expr();
        put('P');
        return;
    }
    switch (cat_of()) {
    case C_NONE:
    case C_OP:
    case C_OPEN:
    case C_NEG:
        put('P');
        return;
    case C_NUM:
        strip_dot();
        put('*');
        put('P');
        return;
    case C_PCT:
    case C_CP:
    case C_CR:
        put('*');
        put('P');
        return;
    default:
        return;
    }
}

static void press_pct(void) {
    if (done) {
        load_value(res_val / 100.0);
        return;
    }
    switch (cat_of()) {
    case C_NUM:
        strip_dot();
        put('%');
        return;
    case C_PI:
        put('%');
        return;
    default:
        return;
    }
}

static void toggle_neg_at(int s) {
    if (s > 0 && buf[s - 1] == 'N') {
        remove_at(s - 1);
    } else {
        insert_at(s, 'N');
    }
}

static void press_sign(void) {
    if (done) {
        load_value(-res_val);
        return;
    }
    struct ctx x;
    scan(&x);
    if (x.in_root) {
        return;
    }
    switch (cat_of()) {
    case C_NUM:
        toggle_neg_at(run_start(len));
        return;
    case C_PCT:
        if (len >= 2 && buf[len - 2] == 'P') {
            toggle_neg_at(len - 2);
        }
        return;
    default:
        return;
    }
}

static void press_open(void) {
    if (done) {
        new_expr();
        put('(');
        return;
    }
    struct ctx x;
    scan(&x);
    if (x.nw > 0 || x.np >= MAX_PAREN) {
        return;
    }
    switch (cat_of()) {
    case C_NONE:
    case C_OP:
    case C_OPEN:
        put('(');
        return;
    case C_PI:
        put('*');
        put('(');
        return;
    default:
        return;
    }
}

static void press_close(void) {
    if (done) {
        return;
    }
    struct ctx x;
    scan(&x);
    if (x.top == 0) {
        return;
    }
    enum cat c = cat_of();
    if (c == C_OP || c == C_NEG) {
        return;
    }
    if (c == C_OPEN) {
        if (x.top == 'R') {
            put('r');
            err = true;
        }
        return;
    }
    strip_dot();
    put(x.top == '(' ? ')' : (x.top == 'R' ? 'r' : 'w'));
}

static void press_bs(void) {
    if (len == 0) {
        return;
    }
    done = false;
    len--;
}

void calc_key(uint8_t k) {
    k_mutex_lock(&calc_lock, K_FOREVER);
    last_input = k_uptime_get_32();
    if (k == KEY_CLR) {
        reset_all();
    } else if (!(full && k != KEY_BS)) {
        if (k <= 9) {
            press_digit(k);
        } else {
            switch (k) {
            case KEY_DOT:
                press_dot();
                break;
            case KEY_ADD:
                press_op('+');
                break;
            case KEY_SUB:
                press_op('-');
                break;
            case KEY_MUL:
                press_op('*');
                break;
            case KEY_DIV:
                press_op('/');
                break;
            case KEY_EQ:
                press_eq();
                break;
            case KEY_SQRT:
                press_sqrt();
                break;
            case KEY_POW:
                press_pow();
                break;
            case KEY_PI:
                press_pi();
                break;
            case KEY_PCT:
                press_pct();
                break;
            case KEY_SIGN:
                press_sign();
                break;
            case KEY_OPEN:
                press_open();
                break;
            case KEY_CLOSE:
                press_close();
                break;
            case KEY_BS:
                press_bs();
                break;
            default:
                break;
            }
        }
    }
    recompute();
    version++;
    k_mutex_unlock(&calc_lock);
}

uint32_t calc_version(void) {
    return version;
}

void calc_get_view(struct calc_view *v) {
    k_mutex_lock(&calc_lock, K_FOREVER);
    build_disp(buf, len, v->disp, sizeof(v->disp));
    if (err) {
        snprintf(v->res, sizeof(v->res), "ERROR");
    } else if (full) {
        snprintf(v->res, sizeof(v->res), "FULL");
    } else if (done) {
        snprintf(v->res, sizeof(v->res), "=%s", res_txt);
    } else {
        snprintf(v->res, sizeof(v->res), "%s", interm);
    }
    uint32_t idle = k_uptime_get_32() - last_input;
    bool phase = (idle >= 5000) || (((idle / 500) % 2) == 0);
    v->cursor = !done && !err && !full && phase;
    k_mutex_unlock(&calc_lock);
}

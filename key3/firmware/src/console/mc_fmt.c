/* mc_fmt -- see mc_fmt.h. Integer-only formatter; C11, freestanding-safe. */
#include "mc_fmt.h"

static void emit_char(char *buf, size_t cap, size_t *pos, char c) {
    if (*pos + 1u < cap) buf[*pos] = c;      /* keep room for the NUL */
    (*pos)++;
}
static void emit_repeat(char *buf, size_t cap, size_t *pos, char c, unsigned n) {
    while (n--) emit_char(buf, cap, pos, c);
}
static void emit_str(char *buf, size_t cap, size_t *pos, const char *s) {
    while (*s) emit_char(buf, cap, pos, *s++);
}

/* Digits of v in base, least significant first; returns the count. */
static unsigned emit_digits(unsigned v, unsigned base, int upper, char *tmp) {
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    unsigned n = 0;
    do {
        tmp[n++] = digits[v % base];
        v /= base;
    } while (v);
    return n;
}

int mc_vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap) {
    size_t pos = 0;
    if (!buf || !cap) return 0;
    for (const char *p = fmt; *p; ++p) {
        if (*p != '%') { emit_char(buf, cap, &pos, *p); continue; }
        ++p;
        int left = 0;
        char pad = ' ';
        if (*p == '-') { left = 1; ++p; }
        if (*p == '0') { pad = '0'; ++p; }
        unsigned width = 0;
        while (*p >= '0' && *p <= '9') { width = width * 10u + (unsigned)(*p - '0'); ++p; }
        char c = *p;
        if (c == '\0') break;
        if (c == '%') { emit_char(buf, cap, &pos, '%'); continue; }
        if (c == 's') {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            unsigned len = 0;
            while (s[len]) len++;
            if (!left && width > len) emit_repeat(buf, cap, &pos, ' ', width - len);
            emit_str(buf, cap, &pos, s);
            if (left && width > len) emit_repeat(buf, cap, &pos, ' ', width - len);
            continue;
        }
        if (c == 'c') {
            char ch = (char)va_arg(ap, int);
            if (!left && width > 1u) emit_repeat(buf, cap, &pos, ' ', width - 1u);
            emit_char(buf, cap, &pos, ch);
            if (left && width > 1u) emit_repeat(buf, cap, &pos, ' ', width - 1u);
            continue;
        }
        if (c == 'd' || c == 'u' || c == 'x' || c == 'X') {
            int neg = 0;
            unsigned v;
            char tmp[10];
            unsigned n;
            if (c == 'd') {
                int sv = va_arg(ap, int);
                if (sv < 0) { neg = 1; v = (unsigned)(-(sv + 1)) + 1u; }
                else v = (unsigned)sv;
                n = emit_digits(v, 10u, 0, tmp);
            } else {
                v = va_arg(ap, unsigned);
                n = (c == 'u') ? emit_digits(v, 10u, 0, tmp)
                               : emit_digits(v, 16u, c == 'X', tmp);
            }
            {
                unsigned body = n + (neg ? 1u : 0u);
                if (!left && pad == ' ' && width > body) emit_repeat(buf, cap, &pos, ' ', width - body);
                if (neg) emit_char(buf, cap, &pos, '-');
                if (!left && pad == '0' && width > body) emit_repeat(buf, cap, &pos, '0', width - body);
                while (n) emit_char(buf, cap, &pos, tmp[--n]);
                if (left && width > body) emit_repeat(buf, cap, &pos, ' ', width - body);
            }
            continue;
        }
        /* Unknown specifier: emit it literally so nothing is silently lost. */
        emit_char(buf, cap, &pos, '%');
        emit_char(buf, cap, &pos, c);
    }
    buf[pos < cap ? pos : cap - 1u] = '\0';
    return (int)pos;
}

int mc_snprintf(char *buf, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = mc_vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return r;
}

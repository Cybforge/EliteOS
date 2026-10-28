#include <kernel.h>
#include <spinlock.h>
#include <dev.h>

/* ------------------------------------------------------------------------
 * vsnprintf
 * ---------------------------------------------------------------------- */

struct out {
    char *buf;
    size_t size;
    size_t pos;
};

static void put(struct out *o, char c)
{
    if (o->pos + 1 < o->size) o->buf[o->pos] = c;
    o->pos++;
}

static void put_str(struct out *o, const char *s, int len, int width, int left)
{
    int pad = width > len ? width - len : 0;
    if (!left) while (pad--) put(o, ' ');
    for (int i = 0; i < len; i++) put(o, s[i]);
    if (left) while (pad-- > 0) put(o, ' ');
}

static void put_num(struct out *o, uint64_t v, int neg, int base, int upper,
                    int width, int prec, int left, int zero, char sign)
{
    char tmp[72];
    int n = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (!v && prec != 0) tmp[n++] = '0';
    while (v) { tmp[n++] = digits[v % (unsigned)base]; v /= (unsigned)base; }
    while (n < prec) tmp[n++] = '0';
    char sgn = neg ? '-' : sign;
    int len = n + (sgn ? 1 : 0);
    int pad = width > len ? width - len : 0;
    if (!left && !zero) while (pad-- > 0) put(o, ' ');
    if (sgn) put(o, sgn);
    if (!left && zero) while (pad-- > 0) put(o, '0');
    while (n) put(o, tmp[--n]);
    if (left) while (pad-- > 0) put(o, ' ');
}

static void put_double(struct out *o, double d, int prec, int width, int left, int zero, char sign)
{
    if (prec < 0) prec = 6;
    if (prec > 9) prec = 9;
    int neg = d < 0;
    if (neg) d = -d;
    uint64_t scale = 1;
    for (int i = 0; i < prec; i++) scale *= 10;
    uint64_t whole = (uint64_t)d;
    uint64_t frac = (uint64_t)((d - (double)whole) * (double)scale + 0.5);
    if (frac >= scale) { whole++; frac -= scale; }

    char tmp[64];
    int n = 0;
    if (prec) {
        for (int i = 0; i < prec; i++) { tmp[n++] = (char)('0' + frac % 10); frac /= 10; }
        tmp[n++] = '.';
    }
    if (!whole) tmp[n++] = '0';
    while (whole && n < 60) { tmp[n++] = (char)('0' + whole % 10); whole /= 10; }
    char sgn = neg ? '-' : sign;
    int len = n + (sgn ? 1 : 0);
    int pad = width > len ? width - len : 0;
    if (!left && !zero) while (pad-- > 0) put(o, ' ');
    if (sgn) put(o, sgn);
    if (!left && zero) while (pad-- > 0) put(o, '0');
    while (n) put(o, tmp[--n]);
    if (left) while (pad-- > 0) put(o, ' ');
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    struct out o = { buf, size, 0 };
    for (; *fmt; fmt++) {
        if (*fmt != '%') { put(&o, *fmt); continue; }
        fmt++;
        int left = 0, zero = 0, width = 0, prec = -1, lng = 0, alt = 0;
        char sign = 0;
        for (;; fmt++) {
            if (*fmt == '-') left = 1;
            else if (*fmt == '#') alt = 1;
            else if (*fmt == '0') zero = 1;
            else if (*fmt == '+') sign = '+';
            else if (*fmt == ' ') { if (!sign) sign = ' '; }
            else break;
        }
        if (*fmt == '*') { width = va_arg(ap, int); if (width < 0) { left = 1; width = -width; } fmt++; }
        else while (isdigit(*fmt)) width = width * 10 + (*fmt++ - '0');
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
            else while (isdigit(*fmt)) prec = prec * 10 + (*fmt++ - '0');
        }
        while (*fmt == 'l' || *fmt == 'z' || *fmt == 'h' || *fmt == 'j' || *fmt == 't') {
            if (*fmt == 'l' || *fmt == 'z' || *fmt == 'j' || *fmt == 't') lng = 1;
            fmt++;
        }
        switch (*fmt) {
        case 'd': case 'i': {
            int64_t v = lng ? va_arg(ap, int64_t) : va_arg(ap, int);
            put_num(&o, v < 0 ? (uint64_t)(-v) : (uint64_t)v, v < 0, 10, 0, width, prec, left, zero && prec < 0, sign);
            break;
        }
        case 'u': case 'x': case 'X': case 'o': case 'b': {
            uint64_t v = lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned);
            int base = *fmt == 'u' ? 10 : *fmt == 'o' ? 8 : *fmt == 'b' ? 2 : 16;
            if (alt && base == 16) { put(&o, '0'); put(&o, *fmt == 'X' ? 'X' : 'x'); if (width >= 2) width -= 2; }
            put_num(&o, v, 0, base, *fmt == 'X', width, prec, left, zero && prec < 0, 0);
            break;
        }
        case 'p': {
            uint64_t v = (uint64_t)va_arg(ap, void *);
            put(&o, '0'); put(&o, 'x');
            put_num(&o, v, 0, 16, 0, 16, -1, 0, 1, 0);
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            put_str(&o, &c, 1, width, left);
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int len = (int)(prec >= 0 ? strnlen(s, (size_t)prec) : strlen(s));
            put_str(&o, s, len, width, left);
            break;
        }
        case 'f': case 'g': case 'e': {
            double d = va_arg(ap, double);
            put_double(&o, d, prec, width, left, zero, sign);
            break;
        }
        case '%':
            put(&o, '%');
            break;
        case 0:
            fmt--;
            break;
        default:
            put(&o, '%');
            put(&o, *fmt);
        }
    }
    if (size) o.buf[o.pos < size ? o.pos : size - 1] = 0;
    return (int)o.pos;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return r;
}

/* ------------------------------------------------------------------------
 * kernel log
 * ---------------------------------------------------------------------- */

#define KLOG_SIZE (64 * 1024)
static char klog_buf[KLOG_SIZE];
static size_t klog_head;          /* total bytes ever written */
static spinlock_t klog_lock = SPINLOCK_INIT("klog");

static void klog_put(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        klog_buf[(klog_head + i) % KLOG_SIZE] = s[i];
    klog_head += n;
}

size_t klog_read(char *buf, size_t size)
{
    spin_lock(&klog_lock);
    size_t avail = klog_head < KLOG_SIZE ? klog_head : KLOG_SIZE;
    if (avail > size - 1) avail = size - 1;
    size_t start = klog_head - avail;
    for (size_t i = 0; i < avail; i++) buf[i] = klog_buf[(start + i) % KLOG_SIZE];
    buf[avail] = 0;
    spin_unlock(&klog_lock);
    return avail;
}

void kprintf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(buf) - 1) n = sizeof(buf) - 1;
    spin_lock(&klog_lock);
    serial_write(buf, (size_t)n);
    klog_put(buf, (size_t)n);
    spin_unlock(&klog_lock);
    bootcon_write(buf, (size_t)n);
}

void klog(const char *fmt, ...)
{
    char buf[512];
    int p = snprintf(buf, sizeof(buf), "[%5u.%03u] ",
                     (unsigned)(timer_ticks / 1000), (unsigned)(timer_ticks % 1000));
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + p, sizeof(buf) - (size_t)p, fmt, ap);
    va_end(ap);
    n += p;
    if (n > (int)sizeof(buf) - 2) n = sizeof(buf) - 2;
    buf[n++] = '\n';
    buf[n] = 0;
    spin_lock(&klog_lock);
    serial_write(buf, (size_t)n);
    klog_put(buf, (size_t)n);
    spin_unlock(&klog_lock);
    bootcon_write(buf, (size_t)n);
}

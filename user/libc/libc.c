/* ZenithOS user-space C library. */
#include "zenith.h"

/* ------------------------------------------------------------------------
 * system calls
 * ---------------------------------------------------------------------- */

static inline long sc(long n, long a, long b, long c, long d, long e)
{
    long ret;
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8)
                     : "rcx", "r11", "memory");
    return ret;
}

void exit(int code)
{
    sc(SYS_EXIT, code, 0, 0, 0, 0);
    for (;;) {}
}

int getpid(void) { return (int)sc(SYS_GETPID, 0, 0, 0, 0, 0); }
int spawn(const char *path, char **argv) { return (int)sc(SYS_SPAWN, (long)path, (long)argv, 0, 0, 0); }
int wait(int pid) { return (int)sc(SYS_WAIT, pid, 0, 0, 0, 0); }
void sleep_ms(unsigned long ms) { sc(SYS_SLEEP, (long)ms, 0, 0, 0, 0); }
unsigned long uptime(void) { return (unsigned long)sc(SYS_UPTIME, 0, 0, 0, 0, 0); }
void yield(void) { sc(SYS_YIELD, 0, 0, 0, 0, 0); }
unsigned int random(void) { return (unsigned int)sc(SYS_RANDOM, 0, 0, 0, 0, 0); }
int get_time(struct z_time *t) { return (int)sc(SYS_TIME, (long)t, 0, 0, 0, 0); }
int sysinfo(struct z_sysinfo *si) { return (int)sc(SYS_SYSINFO, (long)si, 0, 0, 0, 0); }
int open(const char *p, int flags) { return (int)sc(SYS_OPEN, (long)p, flags, 0, 0, 0); }
int close(int fd) { return (int)sc(SYS_CLOSE, fd, 0, 0, 0, 0); }
long read(int fd, void *b, size_t n) { return sc(SYS_READ, fd, (long)b, (long)n, 0, 0); }
long write(int fd, const void *b, size_t n) { return sc(SYS_WRITE, fd, (long)b, (long)n, 0, 0); }
long seek(int fd, long off, int wh) { return sc(SYS_SEEK, fd, off, wh, 0, 0); }
int stat(const char *p, struct z_stat *st) { return (int)sc(SYS_STAT, (long)p, (long)st, 0, 0, 0); }
int readdir(const char *p, int i, struct z_dirent *d) { return (int)sc(SYS_READDIR, (long)p, i, (long)d, 0, 0); }
int mkdir(const char *p) { return (int)sc(SYS_MKDIR, (long)p, 0, 0, 0, 0); }
int unlink(const char *p) { return (int)sc(SYS_UNLINK, (long)p, 0, 0, 0, 0); }
int chdir(const char *p) { return (int)sc(SYS_CHDIR, (long)p, 0, 0, 0, 0); }
int getcwd(char *b, size_t n) { return (int)sc(SYS_GETCWD, (long)b, (long)n, 0, 0, 0); }
void *sbrk(long inc) { return (void *)sc(SYS_SBRK, inc, 0, 0, 0, 0); }

/* ------------------------------------------------------------------------
 * strings
 * ---------------------------------------------------------------------- */

void *memcpy(void *d, const void *s, size_t n)
{
    void *r = d;
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) :: "memory");
    return r;
}

void *memmove(void *d, const void *s, size_t n)
{
    uint8_t *dp = d;
    const uint8_t *sp = s;
    if (dp < sp || dp >= sp + n) return memcpy(d, s, n);
    while (n--) dp[n] = sp[n];
    return d;
}

void *memset(void *d, int c, size_t n)
{
    void *r = d;
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
    return r;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;
    for (size_t i = 0; i < n; i++) if (x[i] != y[i]) return x[i] - y[i];
    return 0;
}

size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (uint8_t)*a - (uint8_t)*b; }
int strncmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) if (a[i] != b[i] || !a[i]) return (uint8_t)a[i] - (uint8_t)b[i];
    return 0;
}
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) {} return r; }
char *strncpy(char *d, const char *s, size_t n)
{
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}
char *strcat(char *d, const char *s) { strcpy(d + strlen(d), s); return d; }
char *strchr(const char *s, int c) { for (; *s; s++) if (*s == (char)c) return (char *)s; return c ? 0 : (char *)s; }
char *strstr(const char *h, const char *n)
{
    size_t l = strlen(n);
    for (; *h; h++) if (!strncmp(h, n, l)) return (char *)h;
    return l ? 0 : (char *)h;
}
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isspace(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
int toupper(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }
int tolower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

long strtol(const char *s, char **end, int base)
{
    long v = 0;
    int neg = 0;
    while (isspace(*s)) s++;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    if (!base) base = 10;
    for (;;) {
        int d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'z') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') d = *s - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * base + d;
        s++;
    }
    if (end) *end = (char *)s;
    return neg ? -v : v;
}

int atoi(const char *s) { return (int)strtol(s, 0, 10); }

/* ------------------------------------------------------------------------
 * formatted output
 * ---------------------------------------------------------------------- */

struct out { char *b; size_t n, pos; };

static void put(struct out *o, char c) { if (o->pos + 1 < o->n) o->b[o->pos] = c; o->pos++; }

static void num(struct out *o, unsigned long v, int base, int neg, int width, int zero, int left)
{
    char t[32];
    int n = 0;
    if (!v) t[n++] = '0';
    while (v) { t[n++] = "0123456789abcdef"[v % (unsigned)base]; v /= (unsigned)base; }
    int len = n + neg;
    int pad = width > len ? width - len : 0;
    if (!left && !zero) while (pad-- > 0) put(o, ' ');
    if (neg) put(o, '-');
    if (!left && zero) while (pad-- > 0) put(o, '0');
    while (n) put(o, t[--n]);
    if (left) while (pad-- > 0) put(o, ' ');
}

int vsnprintf(char *buf, size_t size, const char *f, va_list ap)
{
    struct out o = { buf, size, 0 };
    for (; *f; f++) {
        if (*f != '%') { put(&o, *f); continue; }
        f++;
        int left = 0, zero = 0, width = 0, lng = 0, prec = -1;
        for (;; f++) {
            if (*f == '-') left = 1;
            else if (*f == '0') zero = 1;
            else break;
        }
        while (isdigit(*f)) width = width * 10 + (*f++ - '0');
        if (*f == '.') { f++; prec = 0; while (isdigit(*f)) prec = prec * 10 + (*f++ - '0'); }
        while (*f == 'l' || *f == 'z') { lng = 1; f++; }
        switch (*f) {
        case 'd': case 'i': {
            long v = lng ? va_arg(ap, long) : va_arg(ap, int);
            num(&o, v < 0 ? (unsigned long)-v : (unsigned long)v, 10, v < 0, width, zero, left);
            break;
        }
        case 'u': num(&o, lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned), 10, 0, width, zero, left); break;
        case 'x': num(&o, lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned), 16, 0, width, zero, left); break;
        case 'p': put(&o, '0'); put(&o, 'x'); num(&o, (unsigned long)va_arg(ap, void *), 16, 0, 0, 0, 0); break;
        case 'c': put(&o, (char)va_arg(ap, int)); break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int len = (int)strlen(s);
            if (prec >= 0 && prec < len) len = prec;
            int pad = width > len ? width - len : 0;
            if (!left) while (pad-- > 0) put(&o, ' ');
            for (int i = 0; i < len; i++) put(&o, s[i]);
            if (left) while (pad-- > 0) put(&o, ' ');
            break;
        }
        case 'f': {
            double d = va_arg(ap, double);
            if (prec < 0) prec = 2;
            if (d < 0) { put(&o, '-'); d = -d; }
            unsigned long w = (unsigned long)d;
            num(&o, w, 10, 0, 0, 0, 0);
            if (prec) {
                put(&o, '.');
                double fr = d - (double)w;
                for (int i = 0; i < prec; i++) { fr *= 10; int dg = (int)fr; put(&o, (char)('0' + dg)); fr -= dg; }
            }
            break;
        }
        case '%': put(&o, '%'); break;
        default: put(&o, '%'); put(&o, *f);
        }
    }
    if (size) buf[o.pos < size ? o.pos : size - 1] = 0;
    return (int)o.pos;
}

int snprintf(char *buf, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

int printf(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    write(1, buf, (size_t)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1));
    return n;
}

int putchar(int c) { char ch = (char)c; write(1, &ch, 1); return c; }
int puts(const char *s) { write(1, s, strlen(s)); write(1, "\n", 1); return 0; }

char *readline(char *buf, size_t n)
{
    size_t got = 0;
    while (got + 1 < n) {
        long r = read(0, buf + got, n - 1 - got);
        if (r <= 0) break;
        got += (size_t)r;
        if (buf[got - 1] == '\n') break;
    }
    buf[got] = 0;
    if (got && buf[got - 1] == '\n') buf[got - 1] = 0;
    return got ? buf : 0;
}

/* ------------------------------------------------------------------------
 * malloc: first-fit free list on top of sbrk
 * ---------------------------------------------------------------------- */

struct blk {
    size_t size;        /* payload bytes */
    int free;
    struct blk *next;
};

static struct blk *heap_head;

void *malloc(size_t n)
{
    n = (n + 15) & ~15ul;
    for (struct blk *b = heap_head; b; b = b->next) {
        if (b->free && b->size >= n) {
            if (b->size >= n + sizeof(struct blk) + 32) {
                struct blk *s = (struct blk *)((char *)(b + 1) + n);
                s->size = b->size - n - sizeof(struct blk);
                s->free = 1;
                s->next = b->next;
                b->next = s;
                b->size = n;
            }
            b->free = 0;
            return b + 1;
        }
    }
    size_t grab = n + sizeof(struct blk);
    if (grab < 64 * 1024) grab = 64 * 1024;
    struct blk *b = sbrk((long)grab);
    if ((long)b < 0) return 0;
    b->size = grab - sizeof(struct blk);
    b->free = 1;
    b->next = 0;
    /* append */
    if (!heap_head) heap_head = b;
    else {
        struct blk *t = heap_head;
        while (t->next) t = t->next;
        t->next = b;
    }
    return malloc(n);
}

void free(void *p)
{
    if (!p) return;
    struct blk *b = (struct blk *)p - 1;
    b->free = 1;
    /* merge with following free blocks that are adjacent in memory */
    while (b->next && b->next->free && (char *)(b + 1) + b->size == (char *)b->next) {
        b->size += sizeof(struct blk) + b->next->size;
        b->next = b->next->next;
    }
}

void *calloc(size_t n, size_t sz)
{
    void *p = malloc(n * sz);
    if (p) memset(p, 0, n * sz);
    return p;
}

void *realloc(void *p, size_t n)
{
    if (!p) return malloc(n);
    struct blk *b = (struct blk *)p - 1;
    if (b->size >= n) return p;
    void *q = malloc(n);
    if (q) { memcpy(q, p, b->size); free(p); }
    return q;
}

/* ------------------------------------------------------------------------
 * math
 * ---------------------------------------------------------------------- */

double fabs(double x) { return x < 0 ? -x : x; }
double sqrt(double x) { double r; __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x)); return r; }
double floor(double x) { long i = (long)x; double d = (double)i; return d > x ? d - 1 : d; }

double sin(double x)
{
    const double tp = 2 * M_PI;
    x = x - floor(x / tp) * tp;
    if (x > M_PI) x -= tp;
    if (x > M_PI / 2) x = M_PI - x;
    else if (x < -M_PI / 2) x = -M_PI - x;
    double x2 = x * x;
    return x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72 * (1 - x2 / 110)))));
}

double cos(double x) { return sin(x + M_PI / 2); }

/* ------------------------------------------------------------------------
 * windows
 * ---------------------------------------------------------------------- */

zwin_t *zwin_open(int w, int h, const char *title)
{
    long hnd = sc(SYS_WIN_CREATE, w, h, (long)title, 0, 0);
    if (hnd < 0) return 0;
    zwin_t *z = malloc(sizeof(*z));
    z->handle = (int)hnd;
    z->w = w;
    z->h = h;
    z->px = calloc((size_t)w * h, 4);
    return z;
}

void zwin_present(zwin_t *w) { sc(SYS_WIN_PRESENT, w->handle, (long)w->px, 0, 0, 0); }

int zwin_event(zwin_t *w, struct z_event *ev, int timeout)
{
    return (int)sc(SYS_WIN_EVENT, w->handle, (long)ev, timeout, 0, 0);
}

void zwin_close(zwin_t *w)
{
    sc(SYS_WIN_CLOSE, w->handle, 0, 0, 0, 0);
    free(w->px);
    free(w);
}

void zfill(zwin_t *w, int x, int y, int ww, int hh, uint32_t c)
{
    if (x < 0) { ww += x; x = 0; }
    if (y < 0) { hh += y; y = 0; }
    if (x + ww > w->w) ww = w->w - x;
    if (y + hh > w->h) hh = w->h - y;
    for (int j = 0; j < hh; j++)
        for (int i = 0; i < ww; i++) w->px[(y + j) * w->w + x + i] = c;
}

int ztext(zwin_t *w, int x, int y, const char *text, uint32_t color, int size)
{
    long wh = ((long)w->w << 32) | (uint32_t)w->h;
    long xy = ((long)x << 32) | (uint32_t)y;
    long cs = ((long)size << 32) | color;
    return (int)sc(SYS_DRAW_TEXT, (long)w->px, wh, xy, (long)text, cs);
}

/* ------------------------------------------------------------------------ */

void __libc_init(void) {}

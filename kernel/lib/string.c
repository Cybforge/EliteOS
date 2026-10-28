#include <kernel.h>
#include <mm.h>

void *memcpy(void *dst, const void *src, size_t n)
{
    void *ret = dst;
    if (n >= 64 && !(((uintptr_t)dst | (uintptr_t)src) & 7)) {
        size_t q = n >> 3;
        __asm__ volatile("rep movsq" : "+D"(dst), "+S"(src), "+c"(q) :: "memory");
        n &= 7;
    }
    __asm__ volatile("rep movsb" : "+D"(dst), "+S"(src), "+c"(n) :: "memory");
    return ret;
}

void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    if (d == s || !n) return dst;
    if (d < s || d >= s + n) return memcpy(dst, src, n);
    /* overlapping, copy backwards */
    d += n - 1;
    s += n - 1;
    __asm__ volatile("std; rep movsb; cld" : "+D"(d), "+S"(s), "+c"(n) :: "memory");
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    void *ret = dst;
    if (n >= 64 && !((uintptr_t)dst & 7)) {
        uint64_t v = (uint8_t)c;
        v *= 0x0101010101010101ull;
        size_t q = n >> 3;
        __asm__ volatile("rep stosq" : "+D"(dst), "+c"(q) : "a"(v) : "memory");
        n &= 7;
    }
    __asm__ volatile("rep stosb" : "+D"(dst), "+c"(n) : "a"(c) : "memory");
    return ret;
}

void *memset32(uint32_t *dst, uint32_t v, size_t count)
{
    void *ret = dst;
    __asm__ volatile("rep stosl" : "+D"(dst), "+c"(count) : "a"(v) : "memory");
    return ret;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;
    for (size_t i = 0; i < n; i++)
        if (x[i] != y[i]) return x[i] - y[i];
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

size_t strnlen(const char *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n]) n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (uint8_t)*a - (uint8_t)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i] || !a[i]) return (uint8_t)a[i] - (uint8_t)b[i];
    }
    return 0;
}

int tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
int toupper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int isalnum(int c) { return isdigit(c) || isalpha(c); }
int isspace(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }
int isprint(int c) { return c >= 32 && c < 127; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }

int strcasecmp(const char *a, const char *b)
{
    while (*a && tolower(*a) == tolower(*b)) { a++; b++; }
    return tolower((uint8_t)*a) - tolower((uint8_t)*b);
}

int strncasecmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int x = tolower((uint8_t)a[i]), y = tolower((uint8_t)b[i]);
        if (x != y || !x) return x - y;
    }
    return 0;
}

char *strcpy(char *d, const char *s)
{
    char *r = d;
    while ((*d++ = *s++)) {}
    return r;
}

char *strncpy(char *d, const char *s, size_t n)
{
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}

size_t strlcpy(char *d, const char *s, size_t n)
{
    size_t len = strlen(s);
    if (n) {
        size_t c = len >= n ? n - 1 : len;
        memcpy(d, s, c);
        d[c] = 0;
    }
    return len;
}

size_t strlcat(char *d, const char *s, size_t n)
{
    size_t dl = strnlen(d, n);
    if (dl == n) return n + strlen(s);
    return dl + strlcpy(d + dl, s, n - dl);
}

char *strcat(char *d, const char *s)
{
    strcpy(d + strlen(d), s);
    return d;
}

char *strchr(const char *s, int c)
{
    for (; *s; s++)
        if (*s == (char)c) return (char *)s;
    return c ? NULL : (char *)s;
}

char *strrchr(const char *s, int c)
{
    const char *r = NULL;
    for (; *s; s++)
        if (*s == (char)c) r = s;
    return c ? (char *)r : (char *)s;
}

char *strstr(const char *h, const char *n)
{
    size_t nl = strlen(n);
    if (!nl) return (char *)h;
    for (; *h; h++)
        if (*h == *n && !strncmp(h, n, nl)) return (char *)h;
    return NULL;
}

char *strstr_ci(const char *h, const char *n)
{
    size_t nl = strlen(n);
    if (!nl) return (char *)h;
    for (; *h; h++)
        if (!strncasecmp(h, n, nl)) return (char *)h;
    return NULL;
}

char *strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = kmalloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

long strtol(const char *s, char **end, int base)
{
    long v = 0;
    int neg = 0;
    while (isspace(*s)) s++;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; base = 16; }
    if (base == 0) base = 10;
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

int atoi(const char *s) { return (int)strtol(s, NULL, 10); }

void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = s;
    for (size_t i = 0; i < n; i++)
        if (p[i] == (unsigned char)c) return (void *)(p + i);
    return NULL;
}

static void swap_bytes(unsigned char *a, unsigned char *b, size_t n)
{
    while (n--) { unsigned char t = *a; *a++ = *b; *b++ = t; }
}

static void sift_down(unsigned char *base, size_t size, size_t start, size_t end,
                      int (*cmp)(const void *, const void *))
{
    size_t root = start;
    while (root * 2 + 1 <= end) {
        size_t child = root * 2 + 1, sw = root;
        if (cmp(base + sw * size, base + child * size) < 0) sw = child;
        if (child + 1 <= end && cmp(base + sw * size, base + (child + 1) * size) < 0) sw = child + 1;
        if (sw == root) return;
        swap_bytes(base + root * size, base + sw * size, size);
        root = sw;
    }
}

/* heapsort: no recursion, no allocation, O(n log n) in the worst case */
void qsort(void *b, size_t n, size_t size, int (*cmp)(const void *, const void *))
{
    unsigned char *base = b;
    if (n < 2) return;
    for (size_t start = (n - 2) / 2 + 1; start-- > 0;) sift_down(base, size, start, n - 1, cmp);
    for (size_t end = n - 1; end > 0; end--) {
        swap_bytes(base, base + end * size, size);
        sift_down(base, size, 0, end - 1, cmp);
    }
}

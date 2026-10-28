#ifndef ZENITH_KERNEL_H
#define ZENITH_KERNEL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>

#define ZENITH_NAME     "ZenithOS"
#define ZENITH_VERSION  "1.0"
#define ZENITH_CODENAME "Aurora"

#define ARRAY_SIZE(a)   (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b)       ((a) < (b) ? (a) : (b))
#define MAX(a, b)       ((a) > (b) ? (a) : (b))
#define ABS(x)          ((x) < 0 ? -(x) : (x))
#define ABS_DIFF(a, b)  ((a) > (b) ? (a) - (b) : (b) - (a))
#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))
#define ALIGN_UP(x, a)  (((x) + (a) - 1) & ~((uint64_t)(a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((uint64_t)(a) - 1))
#define UNUSED(x)       ((void)(x))
#define likely(x)       __builtin_expect(!!(x), 1)
#define unlikely(x)     __builtin_expect(!!(x), 0)
#define PACKED          __attribute__((packed))
#define NORETURN        __attribute__((noreturn))

#define KiB(x) ((uint64_t)(x) << 10)
#define MiB(x) ((uint64_t)(x) << 20)
#define GiB(x) ((uint64_t)(x) << 30)

/* lib/string.c */
void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
void  *memset(void *dst, int c, size_t n);
void  *memset32(uint32_t *dst, uint32_t v, size_t count);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t max);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
int    strcasecmp(const char *a, const char *b);
int    strncasecmp(const char *a, const char *b, size_t n);
char  *strcpy(char *d, const char *s);
char  *strncpy(char *d, const char *s, size_t n);
size_t strlcpy(char *d, const char *s, size_t n);
size_t strlcat(char *d, const char *s, size_t n);
char  *strcat(char *d, const char *s);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
char  *strstr(const char *h, const char *n);
char  *strstr_ci(const char *h, const char *n);
char  *strdup(const char *s);
void  *memchr(const void *s, int c, size_t n);
void   qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));
int    atoi(const char *s);
long   strtol(const char *s, char **end, int base);
int    tolower(int c);
int    toupper(int c);
int    isdigit(int c);
int    isalpha(int c);
int    isalnum(int c);
int    isspace(int c);
int    isprint(int c);
int    islower(int c);
int    isupper(int c);

/* lib/printf.c */
int  vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int  snprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void klog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
NORETURN void panic(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* lib/ksym.c */
const char *ksym_lookup(uint64_t addr, uint64_t *off);
uint64_t    ksym_find(const char *name);        /* address of an exported kernel function, 0 if none */

/* kernel log ring buffer (readable from the Log app / dmesg) */
size_t klog_read(char *buf, size_t size);

#define assert(x) do { if (unlikely(!(x))) panic("assertion failed: %s (%s:%d)", #x, __FILE__, __LINE__); } while (0)

/* lib/math.c - small libm for graphics */
double k_sin(double x);
double k_cos(double x);
double k_tan(double x);
double k_atan2(double y, double x);
double k_sqrt(double x);
double k_fabs(double x);
double k_floor(double x);
double k_pow(double b, double e);
double k_exp(double x);
double k_log(double x);
double k_fmod(double a, double b);
#define K_PI 3.14159265358979323846

/* lib/rand.c */
uint32_t krand(void);
void     ksrand(uint64_t seed);

#endif

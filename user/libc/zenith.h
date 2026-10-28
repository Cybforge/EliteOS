/*
 * ZenithOS user-space C library.
 * Programs include this single header and link against libc.
 */
#ifndef ZENITH_USER_H
#define ZENITH_USER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#include "../../kernel/include/syscall.h"

/* process */
void  exit(int code) __attribute__((noreturn));
int   getpid(void);
int   spawn(const char *path, char **argv);
int   wait(int pid);
void  sleep_ms(unsigned long ms);
unsigned long uptime(void);
void  yield(void);
unsigned int random(void);
int   get_time(struct z_time *t);
int   sysinfo(struct z_sysinfo *si);

/* files */
int   open(const char *path, int flags);
int   close(int fd);
long  read(int fd, void *buf, size_t n);
long  write(int fd, const void *buf, size_t n);
long  seek(int fd, long off, int whence);
int   stat(const char *path, struct z_stat *st);
int   readdir(const char *path, int index, struct z_dirent *out);
int   mkdir(const char *path);
int   unlink(const char *path);
int   chdir(const char *path);
int   getcwd(char *buf, size_t n);
void *sbrk(long inc);

/* stdio */
int   putchar(int c);
int   puts(const char *s);
int   printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int   snprintf(char *buf, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int   vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
char *readline(char *buf, size_t n);

/* memory */
void *malloc(size_t n);
void *calloc(size_t n, size_t sz);
void *realloc(void *p, size_t n);
void  free(void *p);

/* strings */
void  *memcpy(void *d, const void *s, size_t n);
void  *memmove(void *d, const void *s, size_t n);
void  *memset(void *d, int c, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strcpy(char *d, const char *s);
char  *strncpy(char *d, const char *s, size_t n);
char  *strcat(char *d, const char *s);
char  *strchr(const char *s, int c);
char  *strstr(const char *h, const char *n);
int    atoi(const char *s);
long   strtol(const char *s, char **end, int base);
int    isdigit(int c);
int    isspace(int c);
int    toupper(int c);
int    tolower(int c);

/* math */
double sin(double x);
double cos(double x);
double sqrt(double x);
double fabs(double x);
double floor(double x);
#define M_PI 3.14159265358979323846

/* windows: a pixel buffer you draw into, then present */
typedef struct {
    int handle;
    int w, h;
    uint32_t *px;
} zwin_t;

enum {
    ZEV_NONE, ZEV_KEY_DOWN, ZEV_KEY_UP, ZEV_MOUSE_MOVE, ZEV_MOUSE_DOWN, ZEV_MOUSE_UP,
    ZEV_MOUSE_WHEEL, ZEV_MOUSE_LEAVE, ZEV_CLOSE, ZEV_RESIZE, ZEV_FOCUS, ZEV_BLUR, ZEV_TIMER,
};

zwin_t *zwin_open(int w, int h, const char *title);
void    zwin_present(zwin_t *w);
int     zwin_event(zwin_t *w, struct z_event *ev, int timeout_ms);   /* 1 = got event */
void    zwin_close(zwin_t *w);
void    zfill(zwin_t *w, int x, int y, int ww, int hh, uint32_t color);
int     ztext(zwin_t *w, int x, int y, const char *text, uint32_t color, int size);

#define ZRGB(r, g, b) (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

#endif

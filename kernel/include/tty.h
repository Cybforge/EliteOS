#ifndef ZENITH_TTY_H
#define ZENITH_TTY_H

#include <kernel.h>
#include <spinlock.h>
#include <wm.h>

#define TTY_KEYS 128
#define TTY_RBUF 4096

struct tty {
    spinlock_t lock;
    void (*out)(void *term, const char *s, size_t n);
    void *term;
    bool raw;                          /* keys go to tty_getkey instead of the line discipline */
    struct gui_event keys[TTY_KEYS];
    unsigned khead, ktail;
    char line[512];
    size_t linelen;
    char rbuf[TTY_RBUF];
    unsigned rhead, rtail;
    bool eof;
    bool closed;
    volatile int fg_pid;
    volatile bool interrupt;
    int cols, rows;
    int refs;
    void (*hangup)(void *term);        /* shell exited */
    void (*release)(void *term);       /* last reference dropped */
};

void tty_init(struct tty *t, void (*out)(void *, const char *, size_t), void *term);
void tty_write(struct tty *t, const char *s, size_t n);
void tty_printf(struct tty *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
long tty_read(struct tty *t, char *buf, size_t n);
bool tty_getkey(struct tty *t, struct gui_event *ev, int timeout_ms);
void tty_input(struct tty *t, const struct gui_event *ev);
void tty_paste(struct tty *t, const char *s);
void tty_close(struct tty *t);
void tty_get(struct tty *t);
void tty_put(struct tty *t);

/* global clipboard */
void clipboard_set(const char *s, size_t n);
char *clipboard_get(void);           /* kmalloc'd copy, may be NULL */

/* shell (apps/shell.c) */
int shell_main(void *arg);

/* expression evaluator (lib/expr.c) */
bool expr_eval(const char *s, double *out, const char **err);
void expr_format(double v, char *buf, size_t n);

#endif

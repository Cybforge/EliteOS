/*
 * Terminal line discipline. The terminal window feeds key events in; the
 * shell reads raw keys, user programs read cooked lines.
 */
void proc_interrupt(int pid);
#include <tty.h>
#include <sched.h>
#include <mm.h>
#include <dev.h>

void tty_init(struct tty *t, void (*out)(void *, const char *, size_t), void *term)
{
    memset(t, 0, sizeof(*t));
    spin_init(&t->lock, "tty");
    t->out = out;
    t->term = term;
    t->raw = true;
    t->cols = 80;
    t->rows = 25;
    t->refs = 1;
}

void tty_write(struct tty *t, const char *s, size_t n)
{
    if (t->closed || !t->out) return;
    t->out(t->term, s, n);
}

void tty_printf(struct tty *t, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    tty_write(t, buf, (size_t)MIN(n, (int)sizeof(buf) - 1));
}

static void push_bytes(struct tty *t, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (t->rhead - t->rtail >= TTY_RBUF) break;
        t->rbuf[t->rhead++ % TTY_RBUF] = s[i];
    }
}

void tty_input(struct tty *t, const struct gui_event *ev)
{
    if (ev->type != EV_KEY_DOWN) return;
    bool ctrl = ev->mods & MOD_CTRL;
    if (ctrl && (ev->key == KEY_C)) {
        t->interrupt = true;
        if (t->fg_pid) proc_interrupt(t->fg_pid);
    }
    spin_lock(&t->lock);
    if (t->raw) {
        if (t->khead - t->ktail < TTY_KEYS) {
            t->keys[t->khead % TTY_KEYS] = *ev;
            t->khead++;
        }
        spin_unlock(&t->lock);
        sched_wake(&t->keys);
        return;
    }
    /* cooked mode: line editing with echo */
    char echo[8];
    size_t elen = 0;
    if (ctrl && ev->key == KEY_D) {
        t->eof = true;
    } else if (ev->key == KEY_ENTER || ev->key == KEY_KPENTER) {
        push_bytes(t, t->line, t->linelen);
        push_bytes(t, "\n", 1);
        t->linelen = 0;
        echo[elen++] = '\n';
    } else if (ev->key == KEY_BACKSPACE) {
        if (t->linelen) {
            t->linelen -= (size_t)utf8_prev_len(t->line, t->line + t->linelen);
            memcpy(echo, "\b \b", 3);
            elen = 3;
        }
    } else if (ev->ch >= 32 && !ctrl) {
        char enc[4];
        int n = utf8_encode(ev->ch, enc);
        if (t->linelen + (size_t)n < sizeof(t->line)) {
            memcpy(t->line + t->linelen, enc, (size_t)n);
            t->linelen += (size_t)n;
            memcpy(echo, enc, (size_t)n);
            elen = (size_t)n;
        }
    }
    spin_unlock(&t->lock);
    if (elen) tty_write(t, echo, elen);
    sched_wake(&t->rbuf);
}

void tty_paste(struct tty *t, const char *s)
{
    struct gui_event ev = { 0 };
    ev.type = EV_KEY_DOWN;
    while (*s) {
        uint32_t cp = utf8_next(&s);
        ev.ch = cp;
        ev.key = cp == '\n' ? KEY_ENTER : 0;
        if (cp == '\n') ev.ch = '\n';
        tty_input(t, &ev);
    }
}

long tty_read(struct tty *t, char *buf, size_t n)
{
    spin_lock(&t->lock);
    while (t->rhead == t->rtail && !t->eof && !t->closed && !t->interrupt)
        sched_wait(&t->rbuf, &t->lock, 100);
    size_t c = 0;
    while (c < n && t->rtail != t->rhead) {
        buf[c++] = t->rbuf[t->rtail++ % TTY_RBUF];
        if (buf[c - 1] == '\n') break;
    }
    if (!c && t->eof) t->eof = false;
    spin_unlock(&t->lock);
    return (long)c;
}

bool tty_getkey(struct tty *t, struct gui_event *ev, int timeout_ms)
{
    uint64_t deadline = timeout_ms > 0 ? uptime_ms() + (uint64_t)timeout_ms : 0;
    spin_lock(&t->lock);
    while (t->khead == t->ktail) {
        if (t->closed || timeout_ms == 0) { spin_unlock(&t->lock); return false; }
        uint64_t now = uptime_ms();
        if (deadline && now >= deadline) { spin_unlock(&t->lock); return false; }
        sched_wait(&t->keys, &t->lock, deadline ? deadline - now : 200);
    }
    *ev = t->keys[t->ktail++ % TTY_KEYS];
    spin_unlock(&t->lock);
    return true;
}

void tty_close(struct tty *t)
{
    spin_lock(&t->lock);
    t->closed = true;
    spin_unlock(&t->lock);
    sched_wake(&t->keys);
    sched_wake(&t->rbuf);
}

void tty_get(struct tty *t)
{
    __atomic_add_fetch(&t->refs, 1, __ATOMIC_ACQ_REL);
}

void tty_put(struct tty *t)
{
    if (__atomic_sub_fetch(&t->refs, 1, __ATOMIC_ACQ_REL) == 0 && t->release)
        t->release(t->term);
}

/* ------------------------------------------------------------------------
 * clipboard
 * ---------------------------------------------------------------------- */

static spinlock_t clip_lock = SPINLOCK_INIT("clipboard");
static char *clip;

void clipboard_set(const char *s, size_t n)
{
    char *c = kmalloc(n + 1);
    if (!c) return;
    memcpy(c, s, n);
    c[n] = 0;
    spin_lock(&clip_lock);
    char *old = clip;
    clip = c;
    spin_unlock(&clip_lock);
    kfree(old);
}

char *clipboard_get(void)
{
    spin_lock(&clip_lock);
    char *r = clip ? strdup(clip) : NULL;
    spin_unlock(&clip_lock);
    return r;
}

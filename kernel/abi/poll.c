/*
 * Waiting for events: poll, select, epoll, and the descriptors that make
 * events (eventfd, timerfd, signalfd, inotify).
 */
#include <kernel.h>
#include <sched.h>
#include <mm.h>
#include <dev.h>
#include <umm.h>
#include <abierr.h>
#include "sv.h"
#include "svcalls.h"

#define POLLIN 1
#define POLLPRI 2
#define POLLOUT 4
#define POLLERR 8
#define POLLHUP 0x10
#define POLLNVAL 0x20
#define O_NONBLOCK 04000
#define O_CLOEXEC 02000000

extern spinlock_t sv_lock;

uint32_t sfile_poll(struct sfile *f, uint32_t events)
{
    if (!f->ops->poll) return events & (POLLIN | POLLOUT);
    return f->ops->poll(f, events);
}

/* ------------------------------------------------------------------------
 * poll, select
 * ---------------------------------------------------------------------- */

struct pollfd { int32_t fd; int16_t events, revents; };

/* one scan: how many descriptors are ready, filling in revents */
static int scan_pollfds(struct sv_group *g, struct pollfd *p, unsigned n)
{
    int ready = 0;
    for (unsigned i = 0; i < n; i++) {
        p[i].revents = 0;
        if (p[i].fd < 0) continue;
        struct sfile *f = sv_fd_get(g, p[i].fd);
        if (!f) { p[i].revents = POLLNVAL; ready++; continue; }
        uint32_t rv = sfile_poll(f, (uint32_t)(uint16_t)p[i].events | POLLERR | POLLHUP);
        sfile_put(f);
        rv &= (uint32_t)(uint16_t)p[i].events | POLLERR | POLLHUP;
        p[i].revents = (int16_t)rv;
        if (rv) ready++;
    }
    return ready;
}

/* the common wait loop; `scan` is called until it reports something, the time is up or a signal arrives */
static long wait_loop(int64_t timeout_ms, int (*scan)(void *), void *ctx)
{
    uint64_t start = uptime_ms();
    for (;;) {
        uint64_t gen = sv_poll_gen();
        int n = scan(ctx);
        if (n) return n;
        if (timeout_ms == 0) return 0;
        uint64_t left = 0;
        if (timeout_ms > 0) {
            uint64_t el = uptime_ms() - start;
            if (el >= (uint64_t)timeout_ms) return 0;
            left = (uint64_t)timeout_ms - el;
        }
        long w = sv_poll_wait_gen(gen, left);
        if (w < 0) return w;
        if (w == 1) { n = scan(ctx); return n; }                 /* one last look at the timeout */
    }
}

struct poll_ctx { struct sv_group *g; struct pollfd *p; unsigned n; };
static int scan_poll(void *c) { struct poll_ctx *x = c; return scan_pollfds(x->g, x->p, x->n); }

static long do_poll(uint64_t ufds, unsigned n, int64_t timeout_ms, uint64_t sigmask, size_t sigsize)
{
    struct sv_group *g = sv_curgrp();
    struct sv_task *t = sv_cur();
    if (n > 65536) return -ERR_INVAL;
    struct pollfd *p = kmalloc(n ? n * sizeof(*p) : 1);
    if (!p) return -ERR_NOMEM;
    if (n && copy_from_user(p, ufds, n * sizeof(*p))) { kfree(p); return -ERR_FAULT; }
    sigset64_t old = t->blocked;
    if (sigmask && sigsize == 8) {
        sigset64_t m;
        if (copy_from_user(&m, sigmask, 8)) { kfree(p); return -ERR_FAULT; }
        t->blocked = m & ~(SIGBIT(SIGKILL) | SIGBIT(SIGSTOP));
    }
    struct poll_ctx c = { g, p, n };
    long r = wait_loop(timeout_ms, scan_poll, &c);
    if (sigmask && sigsize == 8) t->blocked = old;
    if (r >= 0 && n && copy_to_user(ufds, p, n * sizeof(*p))) r = -ERR_FAULT;
    kfree(p);
    return r;
}

SC(s_poll) { return do_poll(r->rdi, (unsigned)r->rsi, (int32_t)r->rdx, 0, 0); }

SC(s_ppoll)
{
    int64_t ms = -1;
    if (r->rdx) {
        struct { int64_t s, ns; } ts;
        if (copy_from_user(&ts, r->rdx, sizeof(ts))) return -ERR_FAULT;
        ms = ts.s * 1000 + (ts.ns + 999999) / 1000000;
    }
    return do_poll(r->rdi, (unsigned)r->rsi, ms, r->r10, r->r8);
}

struct sel_ctx {
    struct sv_group *g;
    int nfds;
    uint64_t *in[3];                 /* the sets the caller gave (read, write, except), kernel copies */
    uint64_t *out[3];
    size_t words;
};

static int scan_select(void *c)
{
    struct sel_ctx *x = c;
    int ready = 0;
    for (int k = 0; k < 3; k++) if (x->out[k]) memset(x->out[k], 0, x->words * 8);
    for (int fd = 0; fd < x->nfds; fd++) {
        uint32_t want = 0;
        for (int k = 0; k < 3; k++)
            if (x->in[k] && (x->in[k][fd / 64] >> (fd % 64) & 1)) want |= k == 0 ? POLLIN : k == 1 ? POLLOUT : POLLPRI;
        if (!want) continue;
        struct sfile *f = sv_fd_get(x->g, fd);
        if (!f) return -ERR_BADF;
        uint32_t rv = sfile_poll(f, want | POLLERR | POLLHUP);
        sfile_put(f);
        for (int k = 0; k < 3; k++) {
            uint32_t bit = k == 0 ? (POLLIN | POLLHUP | POLLERR) : k == 1 ? (POLLOUT | POLLERR) : POLLPRI;
            if (x->in[k] && (x->in[k][fd / 64] >> (fd % 64) & 1) && (rv & bit)) {
                x->out[k][fd / 64] |= 1ull << (fd % 64);
                ready++;
            }
        }
    }
    return ready;
}

static long do_select(int nfds, uint64_t r_, uint64_t w_, uint64_t e_, int64_t timeout_ms, uint64_t sigmask)
{
    if (nfds < 0 || nfds > 65536) return -ERR_INVAL;
    struct sv_task *t = sv_cur();
    struct sel_ctx c;
    memset(&c, 0, sizeof(c));
    c.g = t->g;
    c.nfds = nfds;
    c.words = ((size_t)nfds + 63) / 64;
    uint64_t user[3] = { r_, w_, e_ };
    long rc = 0;
    for (int k = 0; k < 3; k++) {
        if (!user[k]) continue;
        c.in[k] = kzalloc(c.words * 8 + 8);
        c.out[k] = kzalloc(c.words * 8 + 8);
        if (!c.in[k] || !c.out[k]) { rc = -ERR_NOMEM; goto done; }
        size_t nbytes = ((size_t)nfds + 7) / 8;
        if (copy_from_user(c.in[k], user[k], nbytes)) { rc = -ERR_FAULT; goto done; }
    }
    sigset64_t old = t->blocked;
    if (sigmask) {
        sigset64_t m;
        if (copy_from_user(&m, sigmask, 8)) { rc = -ERR_FAULT; goto done; }
        t->blocked = m & ~(SIGBIT(SIGKILL) | SIGBIT(SIGSTOP));
    }
    rc = wait_loop(timeout_ms, scan_select, &c);
    if (sigmask) t->blocked = old;
    if (rc >= 0)
        for (int k = 0; k < 3; k++)
            if (user[k] && copy_to_user(user[k], c.out[k], ((size_t)nfds + 7) / 8)) { rc = -ERR_FAULT; break; }
done:
    for (int k = 0; k < 3; k++) { kfree(c.in[k]); kfree(c.out[k]); }
    return rc;
}

SC(s_select)
{
    int64_t ms = -1;
    if (r->r8) {
        struct { int64_t s, us; } tv;
        if (copy_from_user(&tv, r->r8, sizeof(tv))) return -ERR_FAULT;
        ms = tv.s * 1000 + (tv.us + 999) / 1000;
    }
    return do_select((int)r->rdi, r->rsi, r->rdx, r->r10, ms, 0);
}

SC(s_pselect6)
{
    int64_t ms = -1;
    if (r->r8) {
        struct { int64_t s, ns; } ts;
        if (copy_from_user(&ts, r->r8, sizeof(ts))) return -ERR_FAULT;
        ms = ts.s * 1000 + (ts.ns + 999999) / 1000000;
    }
    uint64_t sm = 0;
    if (r->r9) {
        struct { uint64_t mask, size; } sg;
        if (copy_from_user(&sg, r->r9, sizeof(sg))) return -ERR_FAULT;
        sm = sg.mask;
    }
    return do_select((int)r->rdi, r->rsi, r->rdx, r->r10, ms, sm);
}

/* ------------------------------------------------------------------------
 * epoll
 * ---------------------------------------------------------------------- */

#define EPOLLET (1u << 31)
#define EPOLLONESHOT (1u << 30)

struct interest {
    int fd;
    struct sfile *file;
    uint32_t events;
    uint64_t data;
    uint32_t reported;               /* for edge-triggered: what was ready when last reported */
    bool disabled;
};

struct epoll {
    mutex_t lock;
    struct interest *list;
    int n, cap;
};

struct epoll_event_user { uint32_t events; uint64_t data; } __attribute__((packed));

static uint32_t ep_poll(struct sfile *f, uint32_t ev);

static void ep_close(struct sfile *f)
{
    struct epoll *e = f->priv;
    for (int i = 0; i < e->n; i++) sfile_put(e->list[i].file);
    kfree(e->list);
    kfree(e);
}

/* readiness of one interest; also notices that the descriptor was closed everywhere else (only we hold the file) */
static uint32_t interest_ready(struct interest *in)
{
    if (in->disabled) return 0;
    uint32_t want = in->events & ~(EPOLLET | EPOLLONESHOT);
    return sfile_poll(in->file, want | POLLERR | POLLHUP) & (want | POLLERR | POLLHUP);
}

static void ep_prune(struct epoll *e)
{
    for (int i = 0; i < e->n;) {
        if (e->list[i].file->refs == 1) {                         /* closed by every descriptor: Linux drops it from the set */
            sfile_put(e->list[i].file);
            e->list[i] = e->list[--e->n];
        } else i++;
    }
}

static uint32_t ep_poll(struct sfile *f, uint32_t ev)
{
    struct epoll *e = f->priv;
    uint32_t r = 0;
    mutex_lock(&e->lock);
    ep_prune(e);
    for (int i = 0; i < e->n && !r; i++) if (interest_ready(&e->list[i])) r = POLLIN;
    mutex_unlock(&e->lock);
    return r & ev;
}

static const struct sfile_ops epoll_ops = { .poll = ep_poll, .close = ep_close, .name = "epoll" };

static long make_epoll(int flags)
{
    struct epoll *e = kzalloc(sizeof(*e));
    struct sfile *f = sfile_new(&epoll_ops, 2);
    if (!e || !f) { kfree(e); sfile_put(f); return -ERR_NOMEM; }
    e->lock = (mutex_t)MUTEX_INIT("epoll");
    f->priv = e;
    strlcpy(f->path, "anon_inode:[eventpoll]", sizeof(f->path));
    int fd = sv_fd_alloc(sv_curgrp(), f, 0, (flags & O_CLOEXEC) != 0);
    if (fd < 0) sfile_put(f);
    return fd;
}

SC(s_epoll_create) { return r->rdi ? make_epoll(0) : -ERR_INVAL; }
SC(s_epoll_create1) { return make_epoll((int)r->rdi); }

SC(s_epoll_ctl)
{
    struct sv_group *g = sv_curgrp();
    struct sfile *ef = sv_fd_get(g, (int)r->rdi);
    if (!ef) return -ERR_BADF;
    if (ef->ops != &epoll_ops) { sfile_put(ef); return -ERR_INVAL; }
    int op = (int)r->rsi, fd = (int)r->rdx;
    struct sfile *tf = sv_fd_get(g, fd);
    long rc = 0;
    if (!tf) { sfile_put(ef); return -ERR_BADF; }
    if (tf == ef || tf->ops == &epoll_ops) { sfile_put(tf); sfile_put(ef); return -ERR_INVAL; }
    struct epoll_event_user ev = { 0, 0 };
    if (op != 2 && copy_from_user(&ev, r->r10, sizeof(ev))) { sfile_put(tf); sfile_put(ef); return -ERR_FAULT; }
    struct epoll *e = ef->priv;
    mutex_lock(&e->lock);
    int idx = -1;
    for (int i = 0; i < e->n; i++) if (e->list[i].fd == fd && e->list[i].file == tf) idx = i;
    switch (op) {
    case 1:                                                       /* EPOLL_CTL_ADD */
        if (idx >= 0) { rc = -ERR_EXIST; break; }
        if (e->n == e->cap) {
            int nc = e->cap ? e->cap * 2 : 16;
            struct interest *nl = krealloc(e->list, (size_t)nc * sizeof(*nl));
            if (!nl) { rc = -ERR_NOMEM; break; }
            e->list = nl;
            e->cap = nc;
        }
        e->list[e->n++] = (struct interest){ .fd = fd, .file = sfile_get(tf), .events = ev.events, .data = ev.data };
        break;
    case 2:                                                       /* EPOLL_CTL_DEL */
        if (idx < 0) { rc = -ERR_NOENT; break; }
        sfile_put(e->list[idx].file);
        e->list[idx] = e->list[--e->n];
        break;
    case 3:                                                       /* EPOLL_CTL_MOD */
        if (idx < 0) { rc = -ERR_NOENT; break; }
        e->list[idx].events = ev.events;
        e->list[idx].data = ev.data;
        e->list[idx].reported = 0;
        e->list[idx].disabled = false;
        break;
    default: rc = -ERR_INVAL;
    }
    mutex_unlock(&e->lock);
    sfile_put(tf);
    sfile_put(ef);
    sv_poll_wake();
    return rc;
}

struct ep_ctx { struct epoll *e; struct epoll_event_user *out; int max; int n; };

static int scan_epoll(void *c)
{
    struct ep_ctx *x = c;
    struct epoll *e = x->e;
    x->n = 0;
    mutex_lock(&e->lock);
    ep_prune(e);
    for (int i = 0; i < e->n && x->n < x->max; i++) {
        struct interest *in = &e->list[i];
        uint32_t rv = interest_ready(in);
        if (!rv) { in->reported = 0; continue; }
        if ((in->events & EPOLLET) && !(rv & ~in->reported)) continue;           /* nothing new since the last report */
        x->out[x->n].events = rv;
        x->out[x->n].data = in->data;
        x->n++;
        in->reported = rv;
        if (in->events & EPOLLONESHOT) in->disabled = true;
    }
    mutex_unlock(&e->lock);
    return x->n;
}

static long do_epoll_wait(int epfd, uint64_t uevents, int max, int64_t timeout_ms, uint64_t sigmask)
{
    struct sv_task *t = sv_cur();
    if (max <= 0 || max > 65536) return -ERR_INVAL;
    struct sfile *ef = sv_fd_get(t->g, epfd);
    if (!ef) return -ERR_BADF;
    if (ef->ops != &epoll_ops) { sfile_put(ef); return -ERR_INVAL; }
    struct ep_ctx c = { ef->priv, kmalloc((size_t)max * sizeof(struct epoll_event_user)), max, 0 };
    if (!c.out) { sfile_put(ef); return -ERR_NOMEM; }
    sigset64_t old = t->blocked;
    if (sigmask) {
        sigset64_t m;
        if (copy_from_user(&m, sigmask, 8) == 0) t->blocked = m & ~(SIGBIT(SIGKILL) | SIGBIT(SIGSTOP));
    }
    long rc = wait_loop(timeout_ms, scan_epoll, &c);
    if (sigmask) t->blocked = old;
    if (rc > 0 && copy_to_user(uevents, c.out, (size_t)rc * sizeof(struct epoll_event_user))) rc = -ERR_FAULT;
    kfree(c.out);
    sfile_put(ef);
    return rc;
}

SC(s_epoll_wait) { return do_epoll_wait((int)r->rdi, r->rsi, (int)r->rdx, (int32_t)r->r10, 0); }
SC(s_epoll_pwait) { return do_epoll_wait((int)r->rdi, r->rsi, (int)r->rdx, (int32_t)r->r10, r->r8); }
SC(s_epoll_pwait2)
{
    int64_t ms = -1;
    if (r->r10) {
        struct { int64_t s, ns; } ts;
        if (copy_from_user(&ts, r->r10, sizeof(ts))) return -ERR_FAULT;
        ms = ts.s * 1000 + (ts.ns + 999999) / 1000000;
    }
    return do_epoll_wait((int)r->rdi, r->rsi, (int)r->rdx, ms, r->r8);
}

/* ------------------------------------------------------------------------
 * eventfd
 * ---------------------------------------------------------------------- */

struct evfd { spinlock_t lock; uint64_t count; bool semaphore; };

static long ev_read(struct sfile *f, void *buf, size_t n, uint64_t *off)
{
    UNUSED(off);
    struct evfd *e = f->priv;
    if (n < 8) return -ERR_INVAL;
    spin_lock(&e->lock);
    while (!e->count) {
        if (f->flags & O_NONBLOCK) { spin_unlock(&e->lock); return -ERR_AGAIN; }
        spin_unlock(&e->lock);
        uint64_t gen = sv_poll_gen();
        spin_lock(&e->lock);
        if (e->count) break;
        spin_unlock(&e->lock);
        long w = sv_poll_wait_gen(gen, 0);
        if (w < 0) return w;
        spin_lock(&e->lock);
    }
    uint64_t v = e->semaphore ? 1 : e->count;
    e->count -= v;
    spin_unlock(&e->lock);
    memcpy(buf, &v, 8);
    sv_poll_wake();
    return 8;
}

static long ev_write(struct sfile *f, const void *buf, size_t n, uint64_t *off)
{
    UNUSED(off);
    struct evfd *e = f->priv;
    if (n < 8) return -ERR_INVAL;
    uint64_t v;
    memcpy(&v, buf, 8);
    if (v == ~0ull) return -ERR_INVAL;
    spin_lock(&e->lock);
    if (e->count > 0xFFFFFFFFFFFFFFFEull - v) { spin_unlock(&e->lock); return -ERR_AGAIN; }
    e->count += v;
    spin_unlock(&e->lock);
    sv_poll_wake();
    return 8;
}

static uint32_t ev_poll(struct sfile *f, uint32_t ev)
{
    struct evfd *e = f->priv;
    uint32_t r = 0;
    spin_lock(&e->lock);
    if (e->count) r |= POLLIN;
    if (e->count < 0xFFFFFFFFFFFFFFFEull) r |= POLLOUT;
    spin_unlock(&e->lock);
    return r & (ev | POLLERR);
}

static void ev_close(struct sfile *f) { kfree(f->priv); }

static const struct sfile_ops evfd_ops = { .read = ev_read, .write = ev_write, .poll = ev_poll, .close = ev_close, .name = "eventfd" };

static long make_eventfd(uint32_t init, int flags)
{
    struct evfd *e = kzalloc(sizeof(*e));
    struct sfile *f = sfile_new(&evfd_ops, 2 | (flags & O_NONBLOCK));
    if (!e || !f) { kfree(e); sfile_put(f); return -ERR_NOMEM; }
    spin_init(&e->lock, "eventfd");
    e->count = init;
    e->semaphore = (flags & 1) != 0;
    f->priv = e;
    strlcpy(f->path, "anon_inode:[eventfd]", sizeof(f->path));
    int fd = sv_fd_alloc(sv_curgrp(), f, 0, (flags & O_CLOEXEC) != 0);
    if (fd < 0) sfile_put(f);
    return fd;
}

SC(s_eventfd) { return make_eventfd((uint32_t)r->rdi, 0); }
SC(s_eventfd2) { return make_eventfd((uint32_t)r->rdi, (int)r->rsi); }

/* ------------------------------------------------------------------------
 * timerfd
 * ---------------------------------------------------------------------- */

struct tfd {
    struct tfd *next;
    spinlock_t lock;
    int clock;
    uint64_t next_ns, interval_ns;
    uint64_t expirations;
    bool armed;
    int refs;
};

static struct tfd *tfds;
static spinlock_t tfd_lock = SPINLOCK_INIT("timerfds");

/* called by the timer thread */
void sv_timerfd_tick(uint64_t now_mono_ns)
{
    bool woke = false;
    spin_lock(&tfd_lock);
    for (struct tfd *t = tfds; t; t = t->next) {
        spin_lock(&t->lock);
        uint64_t now = t->clock == 0 ? now_mono_ns + (sv_realtime_ns() - sv_now_ns()) : now_mono_ns;
        if (t->armed && now >= t->next_ns) {
            if (t->interval_ns) {
                uint64_t k = 1 + (now - t->next_ns) / t->interval_ns;
                t->expirations += k;
                t->next_ns += k * t->interval_ns;
            } else {
                t->expirations++;
                t->armed = false;
            }
            woke = true;
        }
        spin_unlock(&t->lock);
    }
    spin_unlock(&tfd_lock);
    if (woke) sv_poll_wake();
}

static long tfd_read(struct sfile *f, void *buf, size_t n, uint64_t *off)
{
    UNUSED(off);
    struct tfd *t = f->priv;
    if (n < 8) return -ERR_INVAL;
    for (;;) {
        uint64_t gen = sv_poll_gen();
        spin_lock(&t->lock);
        if (t->expirations) {
            uint64_t v = t->expirations;
            t->expirations = 0;
            spin_unlock(&t->lock);
            memcpy(buf, &v, 8);
            return 8;
        }
        spin_unlock(&t->lock);
        if (f->flags & O_NONBLOCK) return -ERR_AGAIN;
        long w = sv_poll_wait_gen(gen, 0);
        if (w < 0) return w;
    }
}

static uint32_t tfd_poll(struct sfile *f, uint32_t ev)
{
    struct tfd *t = f->priv;
    spin_lock(&t->lock);
    uint32_t r = t->expirations ? POLLIN : 0;
    spin_unlock(&t->lock);
    return r & ev;
}

static void tfd_close(struct sfile *f)
{
    struct tfd *t = f->priv;
    spin_lock(&tfd_lock);
    for (struct tfd **pp = &tfds; *pp; pp = &(*pp)->next) if (*pp == t) { *pp = t->next; break; }
    spin_unlock(&tfd_lock);
    kfree(t);
}

static const struct sfile_ops tfd_ops = { .read = tfd_read, .poll = tfd_poll, .close = tfd_close, .name = "timerfd" };

SC(s_timerfd_create)
{
    sv_ensure_timer_thread();
    struct tfd *t = kzalloc(sizeof(*t));
    struct sfile *f = sfile_new(&tfd_ops, 2 | ((int)r->rsi & O_NONBLOCK));
    if (!t || !f) { kfree(t); sfile_put(f); return -ERR_NOMEM; }
    spin_init(&t->lock, "timerfd");
    t->clock = (int)r->rdi == 0 ? 0 : 1;
    f->priv = t;
    strlcpy(f->path, "anon_inode:[timerfd]", sizeof(f->path));
    spin_lock(&tfd_lock);
    t->next = tfds;
    tfds = t;
    spin_unlock(&tfd_lock);
    int fd = sv_fd_alloc(sv_curgrp(), f, 0, ((int)r->rsi & O_CLOEXEC) != 0);
    if (fd < 0) sfile_put(f);
    return fd;
}

struct its { int64_t isec, insec, vsec, vnsec; };

SC(s_timerfd_settime)
{
    struct sfile *f = sv_fd_get(sv_curgrp(), (int)r->rdi);
    if (!f) return -ERR_BADF;
    if (f->ops != &tfd_ops) { sfile_put(f); return -ERR_INVAL; }
    struct tfd *t = f->priv;
    struct its n;
    if (copy_from_user(&n, r->rdx, sizeof(n))) { sfile_put(f); return -ERR_FAULT; }
    uint64_t now = t->clock == 0 ? sv_realtime_ns() : sv_now_ns();
    spin_lock(&t->lock);
    if (r->r10) {
        struct its old;
        memset(&old, 0, sizeof(old));
        old.isec = (int64_t)(t->interval_ns / 1000000000ull);
        old.insec = (int64_t)(t->interval_ns % 1000000000ull);
        if (t->armed && t->next_ns > now) { old.vsec = (int64_t)((t->next_ns - now) / 1000000000ull); old.vnsec = (int64_t)((t->next_ns - now) % 1000000000ull); }
        copy_to_user(r->r10, &old, sizeof(old));
    }
    uint64_t v = (uint64_t)n.vsec * 1000000000ull + (uint64_t)n.vnsec;
    t->interval_ns = (uint64_t)n.isec * 1000000000ull + (uint64_t)n.insec;
    t->expirations = 0;
    t->armed = v != 0;
    t->next_ns = (r->rsi & 1) ? v : now + v;                      /* TFD_TIMER_ABSTIME */
    spin_unlock(&t->lock);
    sfile_put(f);
    sv_poll_wake();
    return 0;
}

SC(s_timerfd_gettime)
{
    struct sfile *f = sv_fd_get(sv_curgrp(), (int)r->rdi);
    if (!f) return -ERR_BADF;
    if (f->ops != &tfd_ops) { sfile_put(f); return -ERR_INVAL; }
    struct tfd *t = f->priv;
    struct its o;
    memset(&o, 0, sizeof(o));
    uint64_t now = t->clock == 0 ? sv_realtime_ns() : sv_now_ns();
    spin_lock(&t->lock);
    o.isec = (int64_t)(t->interval_ns / 1000000000ull);
    o.insec = (int64_t)(t->interval_ns % 1000000000ull);
    if (t->armed && t->next_ns > now) { o.vsec = (int64_t)((t->next_ns - now) / 1000000000ull); o.vnsec = (int64_t)((t->next_ns - now) % 1000000000ull); }
    spin_unlock(&t->lock);
    sfile_put(f);
    return copy_to_user(r->rsi, &o, sizeof(o)) ? -ERR_FAULT : 0;
}

/* ------------------------------------------------------------------------
 * signalfd
 * ---------------------------------------------------------------------- */

struct sigfd { sigset64_t mask; };

static long sfd_read(struct sfile *f, void *buf, size_t n, uint64_t *off)
{
    UNUSED(off);
    struct sigfd *s = f->priv;
    struct sv_task *t = sv_cur();
    if (n < 128) return -ERR_INVAL;
    for (;;) {
        uint64_t gen = sv_poll_gen();
        spin_lock(&sv_lock);
        sigset64_t avail = (t->pending | t->g->pending) & s->mask;
        if (avail) {
            int sig = __builtin_ctzll(avail) + 1;
            if (t->pending & SIGBIT(sig)) t->pending &= ~SIGBIT(sig); else t->g->pending &= ~SIGBIT(sig);
            spin_unlock(&sv_lock);
            uint8_t si[128];
            memset(si, 0, sizeof(si));
            *(uint32_t *)si = (uint32_t)sig;                      /* ssi_signo */
            *(uint32_t *)(si + 16) = t->g->tgid;                  /* ssi_pid */
            *(uint32_t *)(si + 20) = t->g->uid;
            memcpy(buf, si, 128);
            return 128;
        }
        spin_unlock(&sv_lock);
        if (f->flags & O_NONBLOCK) return -ERR_AGAIN;
        long w = sv_poll_wait_gen(gen, 0);
        if (w < 0) return w;
    }
}

static uint32_t sfd_poll(struct sfile *f, uint32_t ev)
{
    struct sigfd *s = f->priv;
    struct sv_task *t = sv_cur();
    spin_lock(&sv_lock);
    bool any = t && ((t->pending | t->g->pending) & s->mask);
    spin_unlock(&sv_lock);
    return (any ? POLLIN : 0) & ev;
}

static void sfd_close(struct sfile *f) { kfree(f->priv); }
static const struct sfile_ops sigfd_ops = { .read = sfd_read, .poll = sfd_poll, .close = sfd_close, .name = "signalfd" };

static long make_signalfd(int fd, uint64_t umask, size_t size, int flags)
{
    struct sv_group *g = sv_curgrp();
    if (size != 8) return -ERR_INVAL;
    sigset64_t m;
    if (copy_from_user(&m, umask, 8)) return -ERR_FAULT;
    m &= ~(SIGBIT(SIGKILL) | SIGBIT(SIGSTOP));
    if (fd >= 0) {
        struct sfile *f = sv_fd_get(g, fd);
        if (!f) return -ERR_BADF;
        if (f->ops != &sigfd_ops) { sfile_put(f); return -ERR_INVAL; }
        ((struct sigfd *)f->priv)->mask = m;
        sfile_put(f);
        return fd;
    }
    struct sigfd *s = kzalloc(sizeof(*s));
    struct sfile *f = sfile_new(&sigfd_ops, 2 | (flags & O_NONBLOCK));
    if (!s || !f) { kfree(s); sfile_put(f); return -ERR_NOMEM; }
    s->mask = m;
    f->priv = s;
    strlcpy(f->path, "anon_inode:[signalfd]", sizeof(f->path));
    int nfd = sv_fd_alloc(g, f, 0, (flags & O_CLOEXEC) != 0);
    if (nfd < 0) sfile_put(f);
    return nfd;
}

SC(s_signalfd) { return make_signalfd((int)r->rdi, r->rsi, r->rdx, 0); }
SC(s_signalfd4) { return make_signalfd((int)r->rdi, r->rsi, r->rdx, (int)r->r10); }

/* ------------------------------------------------------------------------
 * inotify: accepted, never reports anything
 * ---------------------------------------------------------------------- */

static long in_read(struct sfile *f, void *buf, size_t n, uint64_t *off)
{
    UNUSED(buf); UNUSED(n); UNUSED(off);
    if (f->flags & O_NONBLOCK) return -ERR_AGAIN;
    for (;;) {
        long w = sv_poll_wait_gen(sv_poll_gen(), 0);
        if (w < 0) return w;
    }
}

static uint32_t in_poll(struct sfile *f, uint32_t ev) { UNUSED(f); UNUSED(ev); return 0; }
static const struct sfile_ops inotify_ops = { .read = in_read, .poll = in_poll, .name = "inotify" };

SC(s_inotify_init)
{
    struct sfile *f = sfile_new(&inotify_ops, 2);
    if (!f) return -ERR_NOMEM;
    strlcpy(f->path, "anon_inode:inotify", sizeof(f->path));
    int fd = sv_fd_alloc(sv_curgrp(), f, 0, false);
    if (fd < 0) sfile_put(f);
    return fd;
}

SC(s_inotify_add_watch) { static int wd; UNUSED(r); return ++wd; }

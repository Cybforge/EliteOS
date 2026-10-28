/*
 * Pipes.
 */
#include <kernel.h>
#include <sched.h>
#include <mm.h>
#include <umm.h>
#include <abierr.h>
#include "sv.h"
#include "svcalls.h"

#define PIPE_SIZE 65536
#define O_NONBLOCK 04000

struct pipe {
    spinlock_t lock;
    uint8_t *buf;
    size_t head, count;
    int readers, writers;
    int refs;
};

static void pipe_unref(struct pipe *p)
{
    if (__atomic_sub_fetch(&p->refs, 1, __ATOMIC_ACQ_REL) == 0) { kfree(p->buf); kfree(p); }
}

static long pipe_read(struct sfile *f, void *buf, size_t n, uint64_t *off)
{
    UNUSED(off);
    struct pipe *p = f->priv;
    spin_lock(&p->lock);
    while (!p->count) {
        if (!p->writers) { spin_unlock(&p->lock); return 0; }
        if (f->flags & O_NONBLOCK) { spin_unlock(&p->lock); return -ERR_AGAIN; }
        int w = sv_wait_chan(&p->head, &p->lock, 0);
        if (w < 0) { spin_unlock(&p->lock); return w; }
    }
    size_t c = MIN(n, p->count), first = MIN(c, PIPE_SIZE - p->head);
    memcpy(buf, p->buf + p->head, first);
    memcpy((uint8_t *)buf + first, p->buf, c - first);
    p->head = (p->head + c) % PIPE_SIZE;
    p->count -= c;
    spin_unlock(&p->lock);
    sched_wake(&p->count);                                  /* writers waiting for room */
    sv_poll_wake();
    return (long)c;
}

static long pipe_write(struct sfile *f, const void *buf, size_t n, uint64_t *off)
{
    UNUSED(off);
    struct pipe *p = f->priv;
    size_t done = 0;
    spin_lock(&p->lock);
    while (done < n) {
        if (!p->readers) {
            spin_unlock(&p->lock);
            struct sv_task *t = sv_cur();
            if (t) sv_send_signal(t->g, t, SIGPIPE);
            return done ? (long)done : -ERR_PIPE;
        }
        size_t room = PIPE_SIZE - p->count;
        /* a write of up to PIPE_BUF bytes is never split */
        if (!room || (n - done <= 4096 && room < n - done)) {
            if (f->flags & O_NONBLOCK) { spin_unlock(&p->lock); return done ? (long)done : -ERR_AGAIN; }
            int w = sv_wait_chan(&p->count, &p->lock, 0);
            if (w < 0) { spin_unlock(&p->lock); return done ? (long)done : w; }
            continue;
        }
        size_t c = MIN(n - done, room);
        size_t tail = (p->head + p->count) % PIPE_SIZE, first = MIN(c, PIPE_SIZE - tail);
        memcpy(p->buf + tail, (const uint8_t *)buf + done, first);
        memcpy(p->buf, (const uint8_t *)buf + done + first, c - first);
        p->count += c;
        done += c;
        spin_unlock(&p->lock);
        sched_wake(&p->head);                               /* readers */
        sv_poll_wake();
        spin_lock(&p->lock);
    }
    spin_unlock(&p->lock);
    return (long)done;
}

static uint32_t pipe_poll(struct sfile *f, uint32_t ev)
{
    struct pipe *p = f->priv;
    uint32_t r = 0;
    spin_lock(&p->lock);
    if ((f->flags & 3) == 0) {                              /* the read end */
        if (p->count) r |= ev & 1;
        if (!p->writers) r |= 0x10;                         /* POLLHUP */
    } else {
        if (p->count < PIPE_SIZE) r |= ev & 4;
        if (!p->readers) r |= 8;                            /* POLLERR */
    }
    spin_unlock(&p->lock);
    return r;
}

static long pipe_ioctl(struct sfile *f, unsigned long cmd, uint64_t arg)
{
    struct pipe *p = f->priv;
    if (cmd == 0x541B) { int v = (int)p->count; return copy_to_user(arg, &v, 4) ? -ERR_FAULT : 0; }
    return -ERR_NOTTY;
}

static int pipe_stat(struct sfile *f, struct vfs_stat *st, uint32_t *mode, uint64_t *ino)
{
    st->type = VN_FILE;
    st->size = 0;
    st->mtime = 0;
    *mode = 0010600;                                        /* S_IFIFO */
    *ino = (uint64_t)(uintptr_t)f->priv >> 4;
    return 0;
}

static void pipe_close(struct sfile *f)
{
    struct pipe *p = f->priv;
    spin_lock(&p->lock);
    if ((f->flags & 3) == 0) p->readers--; else p->writers--;
    spin_unlock(&p->lock);
    sched_wake(&p->head);
    sched_wake(&p->count);
    sv_poll_wake();
    pipe_unref(p);
}

static const struct sfile_ops pipe_ops = {
    .read = pipe_read, .write = pipe_write, .poll = pipe_poll, .ioctl = pipe_ioctl, .stat = pipe_stat, .close = pipe_close,
    .name = "pipe",
};

static long make_pipe(uint64_t ufds, int flags)
{
    struct sv_group *g = sv_curgrp();
    struct pipe *p = kzalloc(sizeof(*p));
    if (!p) return -ERR_NOMEM;
    p->buf = kmalloc(PIPE_SIZE);
    if (!p->buf) { kfree(p); return -ERR_NOMEM; }
    spin_init(&p->lock, "pipe");
    p->readers = p->writers = 1;
    p->refs = 2;
    struct sfile *rf = sfile_new(&pipe_ops, 0 | (flags & O_NONBLOCK));
    struct sfile *wf = sfile_new(&pipe_ops, 1 | (flags & O_NONBLOCK));
    if (!rf || !wf) { sfile_put(rf); sfile_put(wf); return -ERR_NOMEM; }
    rf->priv = wf->priv = p;
    strlcpy(rf->path, "pipe:[pipe]", sizeof(rf->path));
    strlcpy(wf->path, "pipe:[pipe]", sizeof(wf->path));
    bool cx = flags & 02000000;
    int a = sv_fd_alloc(g, rf, 0, cx);
    if (a < 0) { sfile_put(rf); sfile_put(wf); return a; }
    int b = sv_fd_alloc(g, wf, 0, cx);
    if (b < 0) { sv_fd_close(g, a); sfile_put(wf); return b; }
    int fds[2] = { a, b };
    if (copy_to_user(ufds, fds, sizeof(fds))) { sv_fd_close(g, a); sv_fd_close(g, b); return -ERR_FAULT; }
    return 0;
}

SC(s_pipe) { return make_pipe(r->rdi, 0); }
SC(s_pipe2) { return make_pipe(r->rdi, (int)r->rsi); }

/* two connected ends for kernel use (the display server, helpers) */
struct sfile *sv_pipe_pair(struct sfile **write_end, int flags)
{
    struct pipe *p = kzalloc(sizeof(*p));
    if (!p) return NULL;
    p->buf = kmalloc(PIPE_SIZE);
    spin_init(&p->lock, "pipe");
    p->readers = p->writers = 1;
    p->refs = 2;
    struct sfile *rf = sfile_new(&pipe_ops, 0 | flags), *wf = sfile_new(&pipe_ops, 1 | flags);
    rf->priv = wf->priv = p;
    *write_end = wf;
    return rf;
}

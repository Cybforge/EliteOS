/*
 * Open files of System V programs: file objects (shared by duplicated
 * descriptors and across fork) and the descriptor tables that point at them.
 */
#include <kernel.h>
#include <mm.h>
#include <abierr.h>
#include "sv.h"

#define FD_INITIAL 64
#define FD_MAX 65536

struct sfile *sfile_new(const struct sfile_ops *ops, int flags)
{
    struct sfile *f = kzalloc(sizeof(*f));
    if (!f) return NULL;
    f->refs = 1;
    f->ops = ops;
    f->flags = flags;
    f->lock = (mutex_t)MUTEX_INIT("sfile");
    return f;
}

struct sfile *sfile_get(struct sfile *f)
{
    if (f) __atomic_add_fetch(&f->refs, 1, __ATOMIC_ACQ_REL);
    return f;
}

void sfile_put(struct sfile *f)
{
    if (!f || __atomic_sub_fetch(&f->refs, 1, __ATOMIC_ACQ_REL) > 0) return;
    if (f->ops && f->ops->close) f->ops->close(f);
    if (f->vn) vfs_close(f->vn);
    kfree(f);
}

struct sv_files *sv_files_new(void)
{
    struct sv_files *t = kzalloc(sizeof(*t));
    if (!t) return NULL;
    t->refs = 1;
    t->lock = (mutex_t)MUTEX_INIT("fdtable");
    t->cap = FD_INITIAL;
    t->fd = kzalloc((size_t)t->cap * sizeof(struct sfile *));
    t->cloexec = kzalloc((size_t)t->cap);
    if (!t->fd || !t->cloexec) { kfree(t->fd); kfree(t->cloexec); kfree(t); return NULL; }
    return t;
}

struct sv_files *sv_files_copy(struct sv_files *o)
{
    struct sv_files *t = sv_files_new();
    if (!t) return NULL;
    mutex_lock(&o->lock);
    if (o->cap > t->cap) {
        kfree(t->fd);
        kfree(t->cloexec);
        t->cap = o->cap;
        t->fd = kzalloc((size_t)t->cap * sizeof(struct sfile *));
        t->cloexec = kzalloc((size_t)t->cap);
    }
    for (int i = 0; i < o->cap; i++) {
        t->fd[i] = sfile_get(o->fd[i]);
        t->cloexec[i] = o->cloexec[i];
    }
    mutex_unlock(&o->lock);
    return t;
}

void sv_files_put(struct sv_files *t)
{
    if (!t || __atomic_sub_fetch(&t->refs, 1, __ATOMIC_ACQ_REL) > 0) return;
    for (int i = 0; i < t->cap; i++) sfile_put(t->fd[i]);
    kfree(t->fd);
    kfree(t->cloexec);
    kfree(t);
}

int sv_fd_alloc(struct sv_group *g, struct sfile *f, int minfd, bool cloexec)
{
    struct sv_files *t = g->files;
    int limit = (int)MIN(g->rlim_cur[7], (uint64_t)FD_MAX);          /* RLIMIT_NOFILE */
    mutex_lock(&t->lock);
    for (int i = MAX(minfd, 0);; i++) {
        if (i >= limit) { mutex_unlock(&t->lock); return -ERR_MFILE; }
        if (i >= t->cap) {
            int ncap = MIN(MAX(t->cap * 2, i + 1), FD_MAX);
            struct sfile **nf = kzalloc((size_t)ncap * sizeof(struct sfile *));
            uint8_t *nc = kzalloc((size_t)ncap);
            if (!nf || !nc) { kfree(nf); kfree(nc); mutex_unlock(&t->lock); return -ERR_NOMEM; }
            memcpy(nf, t->fd, (size_t)t->cap * sizeof(struct sfile *));
            memcpy(nc, t->cloexec, (size_t)t->cap);
            kfree(t->fd);
            kfree(t->cloexec);
            t->fd = nf;
            t->cloexec = nc;
            t->cap = ncap;
        }
        if (!t->fd[i]) {
            t->fd[i] = f;
            t->cloexec[i] = cloexec;
            mutex_unlock(&t->lock);
            return i;
        }
    }
}

struct sfile *sv_fd_get(struct sv_group *g, int fd)
{
    struct sv_files *t = g->files;
    if (fd < 0) return NULL;
    mutex_lock(&t->lock);
    struct sfile *f = fd < t->cap ? sfile_get(t->fd[fd]) : NULL;
    mutex_unlock(&t->lock);
    return f;
}

int sv_fd_close(struct sv_group *g, int fd)
{
    struct sv_files *t = g->files;
    if (fd < 0) return -ERR_BADF;
    mutex_lock(&t->lock);
    struct sfile *f = fd < t->cap ? t->fd[fd] : NULL;
    if (f) { t->fd[fd] = NULL; t->cloexec[fd] = 0; }
    mutex_unlock(&t->lock);
    if (!f) return -ERR_BADF;
    sfile_put(f);
    return 0;
}

void sv_files_exec(struct sv_files *t)
{
    for (int i = 0; i < t->cap; i++) {
        mutex_lock(&t->lock);
        struct sfile *f = t->cloexec[i] ? t->fd[i] : NULL;
        if (f) { t->fd[i] = NULL; t->cloexec[i] = 0; }
        mutex_unlock(&t->lock);
        sfile_put(f);
    }
}

/* Open file objects of user processes. */
#ifndef ZENITH_FILE_H
#define ZENITH_FILE_H

#include <vfs.h>
#include <tty.h>

enum { F_NULL, F_LOG, F_TTY, F_VNODE };

struct file {
    int kind;
    int refs;
    int flags;
    uint64_t off;
    vnode_t *vn;
    struct tty *tty;
};

static inline void file_put(struct file *f)
{
    if (--f->refs > 0) return;
    if (f->kind == F_VNODE && f->vn) vfs_close(f->vn);
    if (f->kind == F_TTY && f->tty) tty_put(f->tty);
    kfree(f);
}

#endif

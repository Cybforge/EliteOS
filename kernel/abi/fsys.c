/*
 * File system calls of the System V interface: opening, reading and writing,
 * directories, metadata, descriptors.
 */
#include <kernel.h>
#include <cpu.h>
#include <sched.h>
#include <mm.h>
#include <dev.h>
#include <sys.h>
#include <umm.h>
#include <abierr.h>
#include "sv.h"
#include "svcalls.h"

#define AT_FDCWD (-100)
#define AT_REMOVEDIR 0x200
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_EMPTY_PATH 0x1000

#define O_ACCMODE 3
#define O_CREAT 0100
#define O_EXCL 0200
#define O_TRUNC 01000
#define O_APPEND 02000
#define O_NONBLOCK 04000
#define O_DIRECTORY 0200000
#define O_NOFOLLOW 0400000
#define O_CLOEXEC 02000000
#define O_PATH 010000000

#define S_IFMT 0170000
#define S_IFREG 0100000
#define S_IFDIR 0040000
#define S_IFCHR 0020000
#define S_IFLNK 0120000
#define S_IFIFO 0010000
#define S_IFSOCK 0140000

#define CHUNK (64 * 1024)

/* ------------------------------------------------------------------------
 * paths
 * ---------------------------------------------------------------------- */

static int at_path(int dirfd, uint64_t upath, char *out, bool *empty)
{
    struct sv_group *g = sv_curgrp();
    char in[VFS_PATH_MAX];
    long n = strncpy_from_user(in, upath, sizeof(in));
    if (n < 0) return (int)n;
    if (empty) *empty = n == 0;
    if (n == 0) { out[0] = 0; return empty ? 0 : -ERR_NOENT; }
    const char *base = g->cwd;
    char dirpath[VFS_PATH_MAX];
    if (in[0] != '/' && dirfd != AT_FDCWD) {
        struct sfile *d = sv_fd_get(g, dirfd);
        if (!d) return -ERR_BADF;
        strlcpy(dirpath, d->path, sizeof(dirpath));
        sfile_put(d);
        base = dirpath;
    }
    if (!vfs_resolve(base, in, out, VFS_PATH_MAX)) return -ERR_NAMETOOLONG;
    return 0;
}

/* ------------------------------------------------------------------------
 * file kinds
 * ---------------------------------------------------------------------- */

static uint32_t vnode_mode(vnode_t *vn)
{
    switch (vn->type) {
    case VN_DIR: return S_IFDIR | 0755;
    case VN_DEV: return S_IFCHR | 0666;
    default: break;
    }
    if (vn->size >= 4 && vn->data) {
        if (!memcmp(vn->data, "\x7F" "ELF", 4) || (vn->data[0] == '#' && vn->data[1] == '!')) return S_IFREG | 0755;
    } else if (vn->size >= 4 && vn->mnt) {
        uint8_t h[4];
        if (vfs_read(vn, 0, h, 4) == 4 && (!memcmp(h, "\x7F" "ELF", 4) || (h[0] == '#' && h[1] == '!'))) return S_IFREG | 0755;
    }
    return S_IFREG | 0644;
}

static long vn_read(struct sfile *f, void *buf, size_t n, uint64_t *off)
{
    vnode_t *vn = f->vn;
    if (vn->type == VN_DEV) return vn->dev_read ? vn->dev_read(vn, buf, n, *off) : 0;
    long r = vfs_read(vn, *off, buf, n);
    if (r > 0) *off += (uint64_t)r;
    return r;
}

static long vn_write(struct sfile *f, const void *buf, size_t n, uint64_t *off)
{
    vnode_t *vn = f->vn;
    if (vn->type == VN_DEV) return vn->dev_write ? vn->dev_write(vn, buf, n, *off) : (long)n;
    if (f->flags & O_APPEND) *off = vn->size;
    long r = vfs_write(vn, *off, buf, n);
    if (r > 0) *off += (uint64_t)r;
    return r;
}

static long vn_seek(struct sfile *f, int64_t off, int whence)
{
    int64_t base = 0;
    switch (whence) {
    case 0: base = 0; break;
    case 1: base = (int64_t)f->off; break;
    case 2: base = (int64_t)f->vn->size; break;
    case 3: return off < (int64_t)f->vn->size ? (f->off = (uint64_t)off, off) : -ERR_NXIO;     /* SEEK_DATA */
    case 4: return off <= (int64_t)f->vn->size ? (f->off = f->vn->size, (long)f->vn->size) : -ERR_NXIO;   /* SEEK_HOLE */
    default: return -ERR_INVAL;
    }
    if (base + off < 0) return -ERR_INVAL;
    f->off = (uint64_t)(base + off);
    return (long)f->off;
}

static int vn_stat(struct sfile *f, struct vfs_stat *st, uint32_t *mode, uint64_t *ino)
{
    vnode_t *vn = f->vn;
    st->type = vn->type;
    st->size = vn->size;
    st->mtime = vn->mtime;
    *mode = vnode_mode(vn);
    *ino = (uint64_t)(uintptr_t)vn >> 4;
    return 0;
}

static long vn_ioctl(struct sfile *f, unsigned long cmd, uint64_t arg)
{
    if (cmd == 0x541B) {                                    /* FIONREAD */
        uint64_t left = f->vn->size > f->off ? f->vn->size - f->off : 0;
        int v = (int)MIN(left, 0x7FFFFFFFull);
        return copy_to_user(arg, &v, 4) ? -ERR_FAULT : 0;
    }
    return -ERR_NOTTY;
}

static long vn_truncate(struct sfile *f, uint64_t size) { return vfs_truncate(f->vn, size); }

static uint32_t vn_poll(struct sfile *f, uint32_t ev) { UNUSED(f); return ev & (1 | 4); }

const struct sfile_ops vnode_ops = {
    .read = vn_read, .write = vn_write, .seek = vn_seek, .stat = vn_stat, .ioctl = vn_ioctl, .truncate = vn_truncate,
    .poll = vn_poll, .name = "file",
};

/* ---- directories ---- */

static long dir_getdents(struct sfile *f, uint8_t *buf, size_t n)
{
    size_t o = 0;
    for (;;) {
        char name[64];
        int type;
        uint32_t idx = f->dirpos;
        if (idx == 0) { strlcpy(name, ".", sizeof(name)); type = 4; }
        else if (idx == 1) { strlcpy(name, "..", sizeof(name)); type = 4; }
        else {
            struct vfs_dirent de;
            if (vfs_readdir(f->path, (int)idx - 2, &de) != 0) break;
            strlcpy(name, de.name, sizeof(name));
            type = de.type == VN_DIR ? 4 : de.type == VN_DEV ? 2 : 8;
        }
        size_t nl = strlen(name);
        size_t reclen = ALIGN_UP(19 + nl + 1, 8);
        if (o + reclen > n) { if (!o) return -ERR_INVAL; break; }
        memset(buf + o, 0, reclen);
        uint64_t ino = ((uintptr_t)f->vn >> 4) * 131 + idx + 1;
        memcpy(buf + o, &ino, 8);
        uint64_t doff = idx + 1;
        memcpy(buf + o + 8, &doff, 8);
        *(uint16_t *)(buf + o + 16) = (uint16_t)reclen;
        buf[o + 18] = (uint8_t)type;
        memcpy(buf + o + 19, name, nl + 1);
        o += reclen;
        f->dirpos++;
    }
    return (long)o;
}

static int dir_stat(struct sfile *f, struct vfs_stat *st, uint32_t *mode, uint64_t *ino)
{
    st->type = VN_DIR;
    st->size = 4096;
    st->mtime = f->vn ? f->vn->mtime : 0;
    *mode = S_IFDIR | 0755;
    *ino = (uint64_t)(uintptr_t)f->vn >> 4;
    return 0;
}

static long dir_seek(struct sfile *f, int64_t off, int whence)
{
    if (whence == 0 && off == 0) { f->dirpos = 0; return 0; }
    return 0;
}

static long dir_read(struct sfile *f, void *buf, size_t n, uint64_t *off) { UNUSED(f); UNUSED(buf); UNUSED(n); UNUSED(off); return -ERR_ISDIR; }

const struct sfile_ops dir_ops = { .read = dir_read, .getdents = dir_getdents, .stat = dir_stat, .seek = dir_seek, .name = "dir" };

/* ---- /dev/null and friends are VFS device nodes; this is the descriptor that has none of those ---- */

static long null_read(struct sfile *f, void *b, size_t n, uint64_t *o) { UNUSED(f); UNUSED(b); UNUSED(n); UNUSED(o); return 0; }
static long null_write(struct sfile *f, const void *b, size_t n, uint64_t *o) { UNUSED(f); UNUSED(b); UNUSED(o); return (long)n; }
static uint32_t null_poll(struct sfile *f, uint32_t ev) { UNUSED(f); return ev & (1 | 4); }
const struct sfile_ops null_ops = { .read = null_read, .write = null_write, .poll = null_poll, .name = "null" };

/* ------------------------------------------------------------------------
 * open
 * ---------------------------------------------------------------------- */

int sv_open_path(struct sv_group *g, const char *path, int flags, int mode, struct sfile **out)
{
    UNUSED(mode);
    *out = NULL;
    struct sfile *special = NULL;
    int sp = sv_proc_path(g, path, &special);
    if (sp == 0) { *out = special; return 0; }
    if (sp < 0) return sp;

    vnode_t *vn = vfs_lookup(path) ? vfs_open(path, false) : NULL;
    bool created = false;
    if (!vn) {
        if (!(flags & O_CREAT)) return -ERR_NOENT;
        vn = vfs_open(path, true);
        if (!vn) return -ERR_NOENT;
        created = true;
    } else if ((flags & O_CREAT) && (flags & O_EXCL)) {
        vfs_close(vn);
        return -ERR_EXIST;
    }
    UNUSED(created);
    struct sfile *f;
    if (vn->type == VN_DIR) {
        if ((flags & O_ACCMODE) != 0 && !(flags & O_PATH)) { vfs_close(vn); return -ERR_ISDIR; }
        f = sfile_new(&dir_ops, flags);
    } else {
        if (flags & O_DIRECTORY) { vfs_close(vn); return -ERR_NOTDIR; }
        f = sfile_new(&vnode_ops, flags);
        if (f && (flags & O_TRUNC) && (flags & O_ACCMODE) != 0 && vn->type == VN_FILE) vfs_truncate(vn, 0);
    }
    if (!f) { vfs_close(vn); return -ERR_NOMEM; }
    f->vn = vn;
    strlcpy(f->path, path, sizeof(f->path));
    *out = f;
    return 0;
}

static long open_at(int dirfd, uint64_t upath, int flags, int mode)
{
    struct sv_group *g = sv_curgrp();
    char path[VFS_PATH_MAX];
    int r = at_path(dirfd, upath, path, NULL);
    if (r) return r;
    struct sfile *f;
    r = sv_open_path(g, path, flags, mode, &f);
    if (r) return r;
    int fd = sv_fd_alloc(g, f, 0, flags & O_CLOEXEC);
    if (fd < 0) sfile_put(f);
    return fd;
}

SC(s_open) { return open_at(AT_FDCWD, r->rdi, (int)r->rsi, (int)r->rdx); }
SC(s_openat) { return open_at((int)r->rdi, r->rsi, (int)r->rdx, (int)r->r10); }
SC(s_creat) { return open_at(AT_FDCWD, r->rdi, O_CREAT | O_TRUNC | 1, (int)r->rsi); }

SC(s_close) { return sv_fd_close(sv_curgrp(), (int)r->rdi); }

SC(s_close_range)
{
    struct sv_group *g = sv_curgrp();
    uint32_t lo = (uint32_t)r->rdi, hi = (uint32_t)r->rsi;
    for (uint32_t i = lo; i <= hi && i < (uint32_t)g->files->cap; i++) sv_fd_close(g, (int)i);
    return 0;
}

/* ------------------------------------------------------------------------
 * read and write
 * ---------------------------------------------------------------------- */

static long do_read(struct sfile *f, uint64_t ubuf, size_t n, bool use_off, uint64_t off)
{
    if (!f->ops->read) return -ERR_INVAL;
    if ((f->flags & O_ACCMODE) == 1) return -ERR_BADF;
    size_t chunk = MIN(n, (size_t)CHUNK);
    uint8_t *k = kmalloc(chunk ? chunk : 1);
    if (!k) return -ERR_NOMEM;
    long total = 0;
    while ((size_t)total < n) {
        size_t c = MIN(n - (size_t)total, chunk);
        long got;
        if (use_off) { uint64_t o = off + (uint64_t)total; got = f->ops->read(f, k, c, &o); }
        else {
            mutex_lock(&f->lock);
            got = f->ops->read(f, k, c, &f->off);
            mutex_unlock(&f->lock);
        }
        if (got < 0) { if (!total) total = got; break; }
        if (got == 0) break;
        if (copy_to_user(ubuf + (uint64_t)total, k, (size_t)got)) { if (!total) total = -ERR_FAULT; break; }
        total += got;
        if ((size_t)got < c) break;
        if (f->flags & O_NONBLOCK) { /* nothing more is promised */ }
    }
    kfree(k);
    return total;
}

static long do_write(struct sfile *f, uint64_t ubuf, size_t n, bool use_off, uint64_t off)
{
    if (!f->ops->write) return -ERR_BADF;
    if ((f->flags & O_ACCMODE) == 0) return -ERR_BADF;
    size_t chunk = MIN(n, (size_t)CHUNK);
    uint8_t *k = kmalloc(chunk ? chunk : 1);
    if (!k) return -ERR_NOMEM;
    long total = 0;
    while ((size_t)total < n) {
        size_t c = MIN(n - (size_t)total, chunk);
        if (copy_from_user(k, ubuf + (uint64_t)total, c)) { if (!total) total = -ERR_FAULT; break; }
        long put;
        if (use_off) { uint64_t o = off + (uint64_t)total; put = f->ops->write(f, k, c, &o); }
        else {
            mutex_lock(&f->lock);
            put = f->ops->write(f, k, c, &f->off);
            mutex_unlock(&f->lock);
        }
        if (put < 0) { if (!total) total = put; break; }
        total += put;
        if ((size_t)put < c) break;
    }
    kfree(k);
    return total;
}

SC(s_read)
{
    struct sfile *f = sv_fd_get(sv_curgrp(), (int)r->rdi);
    if (!f) return -ERR_BADF;
    long rc = do_read(f, r->rsi, r->rdx, false, 0);
    sfile_put(f);
    return rc;
}

SC(s_write)
{
    struct sfile *f = sv_fd_get(sv_curgrp(), (int)r->rdi);
    if (!f) return -ERR_BADF;
    long rc = do_write(f, r->rsi, r->rdx, false, 0);
    sfile_put(f);
    return rc;
}

SC(s_pread64)
{
    struct sfile *f = sv_fd_get(sv_curgrp(), (int)r->rdi);
    if (!f) return -ERR_BADF;
    long rc = f->ops == &vnode_ops ? do_read(f, r->rsi, r->rdx, true, r->r10) : -ERR_SPIPE;
    sfile_put(f);
    return rc;
}

SC(s_pwrite64)
{
    struct sfile *f = sv_fd_get(sv_curgrp(), (int)r->rdi);
    if (!f) return -ERR_BADF;
    long rc = f->ops == &vnode_ops ? do_write(f, r->rsi, r->rdx, true, r->r10) : -ERR_SPIPE;
    sfile_put(f);
    return rc;
}

struct iov { uint64_t base, len; };

static long do_vec(struct regs *r, bool write, bool use_off, uint64_t off)
{
    struct sfile *f = sv_fd_get(sv_curgrp(), (int)r->rdi);
    if (!f) return -ERR_BADF;
    uint64_t cnt = r->rdx;
    if (cnt > 1024) { sfile_put(f); return -ERR_INVAL; }
    long total = 0;
    for (uint64_t i = 0; i < cnt; i++) {
        struct iov v;
        if (copy_from_user(&v, r->rsi + i * sizeof(v), sizeof(v))) { if (!total) total = -ERR_FAULT; break; }
        if (!v.len) continue;
        long rc = write ? do_write(f, v.base, v.len, use_off, off + (uint64_t)total) : do_read(f, v.base, v.len, use_off, off + (uint64_t)total);
        if (rc < 0) { if (!total) total = rc; break; }
        total += rc;
        if ((uint64_t)rc < v.len) break;
    }
    sfile_put(f);
    return total;
}

SC(s_readv) { return do_vec(r, false, false, 0); }
SC(s_writev) { return do_vec(r, true, false, 0); }
SC(s_preadv) { return do_vec(r, false, true, r->r10); }
SC(s_pwritev) { return do_vec(r, true, true, r->r10); }

SC(s_lseek)
{
    struct sfile *f = sv_fd_get(sv_curgrp(), (int)r->rdi);
    if (!f) return -ERR_BADF;
    long rc = f->ops->seek ? f->ops->seek(f, (int64_t)r->rsi, (int)r->rdx) : -ERR_SPIPE;
    sfile_put(f);
    return rc;
}

SC(s_ioctl)
{
    struct sfile *f = sv_fd_get(sv_curgrp(), (int)r->rdi);
    if (!f) return -ERR_BADF;
    unsigned long cmd = (unsigned long)r->rsi;
    long rc;
    switch (cmd) {
    case 0x5421: {                                            /* FIONBIO */
        int v;
        if (copy_from_user(&v, r->rdx, 4)) { rc = -ERR_FAULT; break; }
        if (v) f->flags |= O_NONBLOCK; else f->flags &= ~O_NONBLOCK;
        rc = 0;
        break;
    }
    case 0x5451: { struct sv_files *t = sv_curgrp()->files; if ((int)r->rdi < t->cap) t->cloexec[(int)r->rdi] = 1; rc = 0; break; }   /* FIOCLEX */
    case 0x5450: { struct sv_files *t = sv_curgrp()->files; if ((int)r->rdi < t->cap) t->cloexec[(int)r->rdi] = 0; rc = 0; break; }   /* FIONCLEX */
    default: rc = f->ops->ioctl ? f->ops->ioctl(f, cmd, r->rdx) : -ERR_NOTTY;
    }
    sfile_put(f);
    return rc;
}

/* ------------------------------------------------------------------------
 * metadata
 * ---------------------------------------------------------------------- */

struct kstat {
    uint64_t st_dev, st_ino, st_nlink;
    uint32_t st_mode, st_uid, st_gid, pad0;
    uint64_t st_rdev;
    int64_t st_size, st_blksize, st_blocks;
    int64_t st_atime, st_atime_nsec, st_mtime, st_mtime_nsec, st_ctime, st_ctime_nsec;
    int64_t unused[3];
};

static void fill_kstat(struct kstat *k, const struct vfs_stat *st, uint32_t mode, uint64_t ino)
{
    struct sv_group *g = sv_curgrp();
    memset(k, 0, sizeof(*k));
    k->st_dev = 1;
    k->st_ino = ino;
    k->st_nlink = (mode & S_IFMT) == S_IFDIR ? 2 : 1;
    k->st_mode = mode;
    k->st_uid = g->uid;
    k->st_gid = g->gid;
    k->st_size = (int64_t)st->size;
    k->st_blksize = 4096;
    k->st_blocks = (int64_t)((st->size + 511) / 512);
    k->st_atime = k->st_mtime = k->st_ctime = st->mtime;
    if ((mode & S_IFMT) == S_IFCHR) k->st_rdev = 0x0103;
}

/* stat the thing a path names; the special files (/proc, sockets in /run ...) have their own answers */
static int stat_path(const char *path, struct vfs_stat *st, uint32_t *mode, uint64_t *ino)
{
    struct sv_group *g = sv_curgrp();
    struct sfile *sp = NULL;
    int rc = sv_proc_path(g, path, &sp);
    if (rc == 0) {
        int r2 = sp->ops->stat ? sp->ops->stat(sp, st, mode, ino) : -ERR_NOENT;
        sfile_put(sp);
        return r2;
    }
    if (rc < 0) return rc;
    vnode_t *vn = vfs_lookup(path);
    if (!vn) return -ERR_NOENT;
    st->type = vn->type;
    st->size = vn->size;
    st->mtime = vn->mtime;
    *mode = vnode_mode(vn);
    *ino = (uint64_t)(uintptr_t)vn >> 4;
    return 0;
}

static long stat_to_user(uint64_t ubuf, const struct vfs_stat *st, uint32_t mode, uint64_t ino)
{
    struct kstat k;
    fill_kstat(&k, st, mode, ino);
    return copy_to_user(ubuf, &k, sizeof(k)) ? -ERR_FAULT : 0;
}

static long do_stat(int dirfd, uint64_t upath, uint64_t ubuf, int flags)
{
    char path[VFS_PATH_MAX];
    bool empty;
    int rc = at_path(dirfd, upath, path, &empty);
    if (rc) return rc;
    struct vfs_stat st;
    uint32_t mode;
    uint64_t ino;
    if (empty && (flags & AT_EMPTY_PATH)) {                               /* fstat through newfstatat */
        struct sfile *f = sv_fd_get(sv_curgrp(), dirfd);
        if (!f) return -ERR_BADF;
        rc = f->ops->stat ? f->ops->stat(f, &st, &mode, &ino) : -ERR_BADF;
        sfile_put(f);
    } else {
        if (empty) return -ERR_NOENT;
        rc = stat_path(path, &st, &mode, &ino);
    }
    if (rc) return rc;
    return stat_to_user(ubuf, &st, mode, ino);
}

SC(s_stat) { return do_stat(AT_FDCWD, r->rdi, r->rsi, 0); }
SC(s_lstat) { return do_stat(AT_FDCWD, r->rdi, r->rsi, AT_SYMLINK_NOFOLLOW); }
SC(s_newfstatat) { return do_stat((int)r->rdi, r->rsi, r->rdx, (int)r->r10); }

SC(s_fstat)
{
    struct sfile *f = sv_fd_get(sv_curgrp(), (int)r->rdi);
    if (!f) return -ERR_BADF;
    struct vfs_stat st;
    uint32_t mode;
    uint64_t ino;
    int rc = f->ops->stat ? f->ops->stat(f, &st, &mode, &ino) : -ERR_BADF;
    sfile_put(f);
    if (rc) return rc;
    return stat_to_user(r->rsi, &st, mode, ino);
}

struct statx_ts { int64_t sec; uint32_t nsec; int32_t pad; };

SC(s_statx)
{
    int dirfd = (int)r->rdi, flags = (int)r->rdx;
    char path[VFS_PATH_MAX];
    bool empty;
    int rc = at_path(dirfd, r->rsi, path, &empty);
    if (rc) return rc;
    struct vfs_stat st;
    uint32_t mode;
    uint64_t ino;
    if (empty && (flags & AT_EMPTY_PATH)) {
        struct sfile *f = sv_fd_get(sv_curgrp(), dirfd);
        if (!f) return -ERR_BADF;
        rc = f->ops->stat ? f->ops->stat(f, &st, &mode, &ino) : -ERR_BADF;
        sfile_put(f);
    } else {
        rc = empty ? -ERR_NOENT : stat_path(path, &st, &mode, &ino);
    }
    if (rc) return rc;
    uint8_t sx[256];
    memset(sx, 0, sizeof(sx));
    struct sv_group *g = sv_curgrp();
    *(uint32_t *)sx = 0x7FF;                                   /* STATX_BASIC_STATS */
    *(uint32_t *)(sx + 4) = 4096;                              /* blksize */
    *(uint32_t *)(sx + 16) = (mode & S_IFMT) == S_IFDIR ? 2 : 1;
    *(uint32_t *)(sx + 20) = g->uid;
    *(uint32_t *)(sx + 24) = g->gid;
    *(uint16_t *)(sx + 28) = (uint16_t)mode;
    *(uint64_t *)(sx + 32) = ino;
    *(uint64_t *)(sx + 40) = st.size;
    *(uint64_t *)(sx + 48) = (st.size + 511) / 512;
    struct statx_ts t = { st.mtime, 0, 0 };
    memcpy(sx + 64, &t, 16);                                   /* atime */
    memcpy(sx + 96, &t, 16);                                   /* ctime */
    memcpy(sx + 112, &t, 16);                                  /* mtime */
    *(uint32_t *)(sx + 136) = 0;                               /* dev major */
    *(uint32_t *)(sx + 140) = 1;
    return copy_to_user(r->r10, sx, sizeof(sx)) ? -ERR_FAULT : 0;
}

SC(s_statfs)
{
    char path[VFS_PATH_MAX];
    int rc = at_path(AT_FDCWD, r->rdi, path, NULL);
    if (rc) return rc;
    uint64_t total = 0, free_b = 0;
    uint64_t type = 0x01021994;                                /* a RAM file system */
    struct fs_mount *m = vfs_mount_of(path);
    if (m && m->ops && m->ops->statfs) { m->ops->statfs(m, &total, &free_b); type = 0x4d44; }
    else { total = pmm_total_pages() * PAGE_SIZE; free_b = pmm_free_pages() * PAGE_SIZE; }
    uint64_t sf[15];
    memset(sf, 0, sizeof(sf));
    sf[0] = type;
    sf[1] = 4096;
    sf[2] = total / 4096;
    sf[3] = free_b / 4096;
    sf[4] = free_b / 4096;
    sf[5] = 1 << 20;
    sf[6] = 1 << 20;
    sf[9] = 255;
    sf[10] = 4096;
    return copy_to_user(r->rsi, sf, sizeof(sf)) ? -ERR_FAULT : 0;
}

SC(s_fstatfs)
{
    struct sfile *f = sv_fd_get(sv_curgrp(), (int)r->rdi);
    if (!f) return -ERR_BADF;
    sfile_put(f);
    struct regs t = *r;
    uint64_t sf[15];
    memset(sf, 0, sizeof(sf));
    sf[0] = 0x01021994; sf[1] = 4096; sf[2] = pmm_total_pages(); sf[3] = pmm_free_pages(); sf[4] = pmm_free_pages();
    sf[9] = 255; sf[10] = 4096;
    (void)t;
    return copy_to_user(r->rsi, sf, sizeof(sf)) ? -ERR_FAULT : 0;
}

static long do_access(int dirfd, uint64_t upath, int mode)
{
    char path[VFS_PATH_MAX];
    int rc = at_path(dirfd, upath, path, NULL);
    if (rc) return rc;
    struct vfs_stat st;
    uint32_t m;
    uint64_t ino;
    rc = stat_path(path, &st, &m, &ino);
    if (rc) return rc;
    if ((mode & 1) && !(m & 0111)) return -ERR_ACCES;            /* X_OK */
    return 0;
}

SC(s_access) { return do_access(AT_FDCWD, r->rdi, (int)r->rsi); }
SC(s_faccessat) { return do_access((int)r->rdi, r->rsi, (int)r->rdx); }
SC(s_enodata) { UNUSED(r); return -61; }

/* ------------------------------------------------------------------------
 * directories and names
 * ---------------------------------------------------------------------- */

static long do_getdents(struct regs *r)
{
    struct sfile *f = sv_fd_get(sv_curgrp(), (int)r->rdi);
    if (!f) return -ERR_BADF;
    if (!f->ops->getdents) { sfile_put(f); return -ERR_NOTDIR; }
    size_t n = MIN((size_t)r->rdx, (size_t)CHUNK);
    uint8_t *k = kmalloc(n ? n : 1);
    if (!k) { sfile_put(f); return -ERR_NOMEM; }
    mutex_lock(&f->lock);
    long rc = f->ops->getdents(f, k, n);
    mutex_unlock(&f->lock);
    if (rc > 0 && copy_to_user(r->rsi, k, (size_t)rc)) rc = -ERR_FAULT;
    kfree(k);
    sfile_put(f);
    return rc;
}

SC(s_getdents64) { return do_getdents(r); }
SC(s_getdents) { UNUSED(r); return -ERR_NOSYS; }

SC(s_getcwd)
{
    struct sv_group *g = sv_curgrp();
    size_t l = strlen(g->cwd) + 1;
    if (r->rsi < l) return -ERR_RANGE;
    if (copy_to_user(r->rdi, g->cwd, l)) return -ERR_FAULT;
    return (long)l;
}

static void set_cwd(struct sv_group *g, const char *path)
{
    strlcpy(g->cwd, path, sizeof(g->cwd));
    strlcpy(g->proc->cwd, path, sizeof(g->proc->cwd));
}

SC(s_chdir)
{
    char path[VFS_PATH_MAX];
    int rc = at_path(AT_FDCWD, r->rdi, path, NULL);
    if (rc) return rc;
    struct vfs_stat st;
    uint32_t m;
    uint64_t ino;
    rc = stat_path(path, &st, &m, &ino);
    if (rc) return rc;
    if ((m & S_IFMT) != S_IFDIR) return -ERR_NOTDIR;
    set_cwd(sv_curgrp(), path);
    return 0;
}

SC(s_fchdir)
{
    struct sfile *f = sv_fd_get(sv_curgrp(), (int)r->rdi);
    if (!f) return -ERR_BADF;
    long rc = f->ops == &dir_ops ? 0 : -ERR_NOTDIR;
    if (!rc) set_cwd(sv_curgrp(), f->path);
    sfile_put(f);
    return rc;
}

static long do_mkdir(int dirfd, uint64_t upath)
{
    char path[VFS_PATH_MAX];
    int rc = at_path(dirfd, upath, path, NULL);
    if (rc) return rc;
    if (vfs_lookup(path)) return -ERR_EXIST;
    rc = vfs_mkdir(path);
    return rc;
}

SC(s_mkdir) { return do_mkdir(AT_FDCWD, r->rdi); }
SC(s_mkdirat) { return do_mkdir((int)r->rdi, r->rsi); }

static long do_unlink(int dirfd, uint64_t upath, bool dir)
{
    char path[VFS_PATH_MAX];
    int rc = at_path(dirfd, upath, path, NULL);
    if (rc) return rc;
    vnode_t *vn = vfs_lookup(path);
    if (!vn) return -ERR_NOENT;
    if (dir && vn->type != VN_DIR) return -ERR_NOTDIR;
    if (!dir && vn->type == VN_DIR) return -ERR_ISDIR;
    return vfs_unlink(path, false);
}

SC(s_unlink) { return do_unlink(AT_FDCWD, r->rdi, false); }
SC(s_rmdir) { return do_unlink(AT_FDCWD, r->rdi, true); }
SC(s_unlinkat) { return do_unlink((int)r->rdi, r->rsi, (r->rdx & AT_REMOVEDIR) != 0); }

static long do_rename(int d1, uint64_t p1, int d2, uint64_t p2)
{
    char a[VFS_PATH_MAX], b[VFS_PATH_MAX];
    int rc = at_path(d1, p1, a, NULL);
    if (rc) return rc;
    rc = at_path(d2, p2, b, NULL);
    if (rc) return rc;
    if (!vfs_lookup(a)) return -ERR_NOENT;
    vnode_t *dst = vfs_lookup(b);
    if (dst && dst->type != VN_DIR) vfs_unlink(b, false);          /* rename replaces an existing file */
    return vfs_rename(a, b);
}

SC(s_rename) { return do_rename(AT_FDCWD, r->rdi, AT_FDCWD, r->rsi); }
SC(s_renameat) { return do_rename((int)r->rdi, r->rsi, (int)r->rdx, r->r10); }
SC(s_renameat2) { return do_rename((int)r->rdi, r->rsi, (int)r->rdx, r->r10); }

static long do_readlink(int dirfd, uint64_t upath, uint64_t ubuf, size_t n)
{
    struct sv_group *g = sv_curgrp();
    char path[VFS_PATH_MAX];
    int rc = at_path(dirfd, upath, path, NULL);
    if (rc) return rc;
    const char *target = NULL;
    char tmp[VFS_PATH_MAX];
    if (!strcmp(path, "/proc/self/exe") || !strcmp(path, "/proc/thread-self/exe")) target = g->exe;
    else if (!strcmp(path, "/proc/self/cwd")) target = g->cwd;
    else if (!strncmp(path, "/proc/self/fd/", 14)) {
        struct sfile *f = sv_fd_get(g, atoi(path + 14));
        if (!f) return -ERR_NOENT;
        strlcpy(tmp, f->path[0] ? f->path : "pipe:[1]", sizeof(tmp));
        sfile_put(f);
        target = tmp;
    } else if (!vfs_lookup(path) && strncmp(path, "/proc", 5)) return -ERR_NOENT;
    if (!target) return -ERR_INVAL;
    size_t l = MIN(strlen(target), n);
    return copy_to_user(ubuf, target, l) ? -ERR_FAULT : (long)l;
}

SC(s_readlink) { return do_readlink(AT_FDCWD, r->rdi, r->rsi, r->rdx); }
SC(s_readlinkat) { return do_readlink((int)r->rdi, r->rsi, r->rdx, r->r10); }

SC(s_truncate)
{
    char path[VFS_PATH_MAX];
    int rc = at_path(AT_FDCWD, r->rdi, path, NULL);
    if (rc) return rc;
    vnode_t *vn = vfs_open(path, false);
    if (!vn) return -ERR_NOENT;
    rc = vn->type == VN_FILE ? vfs_truncate(vn, r->rsi) : -ERR_ISDIR;
    vfs_close(vn);
    return rc;
}

SC(s_ftruncate)
{
    struct sfile *f = sv_fd_get(sv_curgrp(), (int)r->rdi);
    if (!f) return -ERR_BADF;
    long rc = f->ops->truncate ? f->ops->truncate(f, r->rsi) : -ERR_INVAL;
    sfile_put(f);
    return rc;
}

SC(s_fallocate)
{
    struct sfile *f = sv_fd_get(sv_curgrp(), (int)r->rdi);
    if (!f) return -ERR_BADF;
    long rc = 0;
    uint64_t end = r->rdx + r->r10;
    if (f->ops->truncate && f->vn && end > f->vn->size) rc = f->ops->truncate(f, end);
    sfile_put(f);
    return rc;
}

/* ------------------------------------------------------------------------
 * descriptors
 * ---------------------------------------------------------------------- */

SC(s_dup)
{
    struct sv_group *g = sv_curgrp();
    struct sfile *f = sv_fd_get(g, (int)r->rdi);
    if (!f) return -ERR_BADF;
    int fd = sv_fd_alloc(g, f, 0, false);
    if (fd < 0) sfile_put(f);
    return fd;
}

static long dup_to(int oldfd, int newfd, bool cloexec)
{
    struct sv_group *g = sv_curgrp();
    struct sfile *f = sv_fd_get(g, oldfd);
    if (!f) return -ERR_BADF;
    if (oldfd == newfd) { sfile_put(f); return newfd; }
    if (newfd < 0 || (uint64_t)newfd >= g->rlim_cur[7]) { sfile_put(f); return -ERR_BADF; }
    sv_fd_close(g, newfd);
    struct sv_files *t = g->files;
    mutex_lock(&t->lock);
    while (newfd >= t->cap) {
        int ncap = t->cap * 2;
        struct sfile **nf = kzalloc((size_t)ncap * sizeof(struct sfile *));
        uint8_t *nc = kzalloc((size_t)ncap);
        memcpy(nf, t->fd, (size_t)t->cap * sizeof(struct sfile *));
        memcpy(nc, t->cloexec, (size_t)t->cap);
        kfree(t->fd);
        kfree(t->cloexec);
        t->fd = nf;
        t->cloexec = nc;
        t->cap = ncap;
    }
    t->fd[newfd] = f;
    t->cloexec[newfd] = cloexec;
    mutex_unlock(&t->lock);
    return newfd;
}

SC(s_dup2) { return dup_to((int)r->rdi, (int)r->rsi, false); }
SC(s_dup3)
{
    if ((int)r->rdi == (int)r->rsi) return -ERR_INVAL;
    return dup_to((int)r->rdi, (int)r->rsi, (r->rdx & O_CLOEXEC) != 0);
}

SC(s_fcntl)
{
    struct sv_group *g = sv_curgrp();
    int fd = (int)r->rdi, cmd = (int)r->rsi;
    struct sfile *f = sv_fd_get(g, fd);
    if (!f) return -ERR_BADF;
    long rc = 0;
    switch (cmd) {
    case 0: case 1030: {                                      /* F_DUPFD, F_DUPFD_CLOEXEC */
        int nfd = sv_fd_alloc(g, sfile_get(f), (int)r->rdx, cmd == 1030);
        if (nfd < 0) sfile_put(f);
        rc = nfd;
        break;
    }
    case 1: rc = fd < g->files->cap && g->files->cloexec[fd] ? 1 : 0; break;                 /* F_GETFD */
    case 2: if (fd < g->files->cap) g->files->cloexec[fd] = (r->rdx & 1) != 0; break;       /* F_SETFD */
    case 3: rc = f->flags; break;                                                            /* F_GETFL */
    case 4: f->flags = (f->flags & ~(O_APPEND | O_NONBLOCK)) | ((int)r->rdx & (O_APPEND | O_NONBLOCK)); break;
    case 5: {                                                 /* F_GETLK: nothing holds a lock */
        uint8_t fl[32];
        if (copy_from_user(fl, r->rdx, sizeof(fl))) { rc = -ERR_FAULT; break; }
        *(int16_t *)fl = 2;                                   /* F_UNLCK */
        rc = copy_to_user(r->rdx, fl, sizeof(fl)) ? -ERR_FAULT : 0;
        break;
    }
    case 6: case 7: rc = 0; break;                            /* F_SETLK, F_SETLKW: locks always succeed (single machine, advisory) */
    case 1031: rc = 65536; break;                             /* F_SETPIPE_SZ */
    case 1032: rc = 65536; break;                             /* F_GETPIPE_SZ */
    case 1033: case 1034: rc = 0; break;                      /* seals */
    case 8: case 9: case 10: case 11: rc = 0; break;          /* owner / signal */
    default: rc = -ERR_INVAL;
    }
    sfile_put(f);
    return rc;
}

/* ------------------------------------------------------------------------
 * copying between files
 * ---------------------------------------------------------------------- */

static long copy_between(struct sfile *in, uint64_t *inoff, struct sfile *out, size_t n)
{
    uint8_t *k = kmalloc(CHUNK);
    if (!k) return -ERR_NOMEM;
    long total = 0;
    while ((size_t)total < n) {
        size_t c = MIN(n - (size_t)total, (size_t)CHUNK);
        long got;
        if (inoff) got = in->ops->read ? in->ops->read(in, k, c, inoff) : -ERR_INVAL;
        else { mutex_lock(&in->lock); got = in->ops->read ? in->ops->read(in, k, c, &in->off) : -ERR_INVAL; mutex_unlock(&in->lock); }
        if (got <= 0) { if (!total) total = got; break; }
        mutex_lock(&out->lock);
        long put = out->ops->write ? out->ops->write(out, k, (size_t)got, &out->off) : -ERR_BADF;
        mutex_unlock(&out->lock);
        if (put < 0) { if (!total) total = put; break; }
        total += put;
        if (put < got) break;
    }
    kfree(k);
    return total;
}

SC(s_sendfile)
{
    struct sv_group *g = sv_curgrp();
    struct sfile *out = sv_fd_get(g, (int)r->rdi), *in = sv_fd_get(g, (int)r->rsi);
    long rc;
    if (!in || !out) rc = -ERR_BADF;
    else {
        uint64_t off = 0;
        if (r->rdx) {
            if (copy_from_user(&off, r->rdx, 8)) { rc = -ERR_FAULT; goto done; }
            rc = copy_between(in, &off, out, r->r10);
            if (rc > 0) copy_to_user(r->rdx, &off, 8);
        } else rc = copy_between(in, NULL, out, r->r10);
    }
done:
    sfile_put(in);
    sfile_put(out);
    return rc;
}

SC(s_copy_file_range)
{
    struct sv_group *g = sv_curgrp();
    struct sfile *in = sv_fd_get(g, (int)r->rdi), *out = sv_fd_get(g, (int)r->rdx);
    long rc;
    if (!in || !out) rc = -ERR_BADF;
    else {
        uint64_t io = 0, oo = 0;
        if (r->rsi && copy_from_user(&io, r->rsi, 8)) rc = -ERR_FAULT;
        else if (r->r10 && copy_from_user(&oo, r->r10, 8)) rc = -ERR_FAULT;
        else {
            if (r->r10) out->off = oo;
            rc = copy_between(in, r->rsi ? &io : NULL, out, r->r8);
            if (rc > 0) {
                if (r->rsi) copy_to_user(r->rsi, &io, 8);
                if (r->r10) { uint64_t no = oo + (uint64_t)rc; copy_to_user(r->r10, &no, 8); }
            }
        }
    }
    sfile_put(in);
    sfile_put(out);
    return rc;
}

/* ------------------------------------------------------------------------
 * memfd: a file made of shared memory
 * ---------------------------------------------------------------------- */

static long memfd_read(struct sfile *f, void *buf, size_t n, uint64_t *off)
{
    long r = shm_read(f->priv, *off, buf, n);
    if (r > 0) *off += (uint64_t)r;
    return r;
}

static long memfd_write(struct sfile *f, const void *buf, size_t n, uint64_t *off)
{
    struct shm *s = f->priv;
    if (f->flags & O_APPEND) *off = s->size;
    long r = shm_write(s, *off, buf, n);
    if (r > 0) *off += (uint64_t)r;
    return r;
}

static long memfd_seek(struct sfile *f, int64_t off, int whence)
{
    struct shm *s = f->priv;
    int64_t base = whence == 0 ? 0 : whence == 1 ? (int64_t)f->off : (int64_t)s->size;
    if (whence > 2 || base + off < 0) return -ERR_INVAL;
    f->off = (uint64_t)(base + off);
    return (long)f->off;
}

static long memfd_truncate(struct sfile *f, uint64_t size) { return shm_resize(f->priv, size); }

static int memfd_stat(struct sfile *f, struct vfs_stat *st, uint32_t *mode, uint64_t *ino)
{
    struct shm *s = f->priv;
    st->type = VN_FILE;
    st->size = s->size;
    st->mtime = rtc_epoch();
    *mode = S_IFREG | 0600;
    *ino = (uint64_t)(uintptr_t)s >> 4;
    return 0;
}

static long memfd_mmap(struct sfile *f, struct mm *m, uint64_t addr, uint64_t len, uint32_t prot, uint32_t flags, bool fixed, uint64_t off)
{
    struct shm *s = f->priv;
    if (off + len > ALIGN_UP(s->size, PAGE_SIZE) && (flags & VM_SHARED)) {
        if (shm_resize(s, off + len) && 0) return -ERR_NOMEM;     /* a mapping past the end is allowed; those pages fault (SIGBUS) */
    }
    return mm_mmap(m, addr, len, prot, flags & ~0x80000000u, fixed, false, NULL, off, s, "/memfd");
}

static void memfd_close(struct sfile *f) { shm_put(f->priv); }

static const struct sfile_ops memfd_ops = {
    .read = memfd_read, .write = memfd_write, .seek = memfd_seek, .truncate = memfd_truncate, .stat = memfd_stat,
    .mmap = memfd_mmap, .close = memfd_close, .poll = vn_poll, .name = "memfd",
};

struct sfile *sv_memfd_new(const char *name, uint32_t flags)
{
    UNUSED(flags);
    struct shm *s = shm_new(0, name);
    if (!s) return NULL;
    struct sfile *f = sfile_new(&memfd_ops, 2);
    if (!f) { shm_put(s); return NULL; }
    f->priv = s;
    snprintf(f->path, sizeof(f->path), "/memfd:%s (deleted)", name);
    return f;
}

SC(s_memfd_create)
{
    char name[64];
    if (strncpy_from_user(name, r->rdi, sizeof(name)) < 0) return -ERR_FAULT;
    struct sfile *f = sv_memfd_new(name, (uint32_t)r->rsi);
    if (!f) return -ERR_NOMEM;
    int fd = sv_fd_alloc(sv_curgrp(), f, 0, (r->rsi & 1) != 0);          /* MFD_CLOEXEC */
    if (fd < 0) sfile_put(f);
    return fd;
}

#ifndef ZENITH_VFS_H
#define ZENITH_VFS_H

#include <kernel.h>
#include <spinlock.h>

/* sleeping mutex (proc/mutex.c) */
typedef struct {
    spinlock_t lk;
    volatile int locked;
    void *owner;
} mutex_t;
#define MUTEX_INIT(n) { SPINLOCK_INIT(n), 0, 0 }
void mutex_lock(mutex_t *m);
void mutex_unlock(mutex_t *m);
bool mutex_trylock(mutex_t *m);

enum { VN_FILE = 1, VN_DIR = 2, VN_DEV = 3 };

typedef struct vnode vnode_t;
struct fs_mount;

typedef long (*dev_read_fn)(vnode_t *vn, void *buf, size_t len, uint64_t off);
typedef long (*dev_write_fn)(vnode_t *vn, const void *buf, size_t len, uint64_t off);

struct vnode {
    char name[64];
    int type;
    uint8_t *data;
    size_t size, cap;
    vnode_t *parent, *child, *sibling;
    int64_t mtime;
    dev_read_fn dev_read;
    dev_write_fn dev_write;
    void *dev_ctx;
    int refs;
    bool unlinked;
    /* disk-backed nodes */
    struct fs_mount *mnt;
    uint32_t ino;               /* file system private (FAT: first cluster) */
    bool loaded;                /* file data / directory entries are in memory */
    bool dirty;                 /* needs writing back */
    vnode_t *alias;             /* directory this name stands for (see vfs_alias) */
};

/* a mounted disk file system; every call is made with the VFS lock held */
struct fs_ops {
    int  (*load)(struct fs_mount *m, vnode_t *n);       /* read file data or directory entries */
    void (*release)(struct fs_mount *m, vnode_t *n);    /* node deleted: free its disk space */
    int  (*sync)(struct fs_mount *m);                   /* write every dirty node */
    void (*statfs)(struct fs_mount *m, uint64_t *total, uint64_t *free);
    void (*unmount)(struct fs_mount *m);                /* after the final sync; frees m */
};

struct fs_mount {
    const struct fs_ops *ops;
    void *priv;
    vnode_t *root;
    char path[64];
    char dev[16];
    char label[16];
    char fstype[8];
    struct fs_mount *next;
};

struct vfs_stat {
    int type;
    size_t size;
    int64_t mtime;
};

struct vfs_dirent {
    char name[64];
    int type;
    size_t size;
    int64_t mtime;
};

#define VFS_PATH_MAX 256

void     vfs_init(void);
void     vfs_load_tar(const void *data, size_t size);
bool     vfs_resolve(const char *cwd, const char *path, char *out, size_t outsz);
vnode_t *vfs_lookup(const char *path);
vnode_t *vfs_open(const char *path, bool create);        /* takes a reference */
void     vfs_close(vnode_t *vn);
int      vfs_mkdir(const char *path);
int      vfs_unlink(const char *path, bool recursive);
int      vfs_rename(const char *from, const char *to);
int      vfs_copy(const char *from, const char *to);
long     vfs_read(vnode_t *vn, uint64_t off, void *buf, size_t len);
long     vfs_write(vnode_t *vn, uint64_t off, const void *buf, size_t len);
int      vfs_truncate(vnode_t *vn, size_t size);
int      vfs_stat(const char *path, struct vfs_stat *st);
int      vfs_readdir(const char *path, int index, struct vfs_dirent *out);
int      vfs_list(const char *path, struct vfs_dirent *out, int max);
char    *vfs_read_file(const char *path, size_t *size);   /* kmalloc'd, NUL terminated */
int      vfs_write_file(const char *path, const void *data, size_t size);
vnode_t *vfs_register_dev(const char *path, dev_read_fn r, dev_write_fn w, void *ctx);
size_t   vfs_total_bytes(void);

/* make a directory appear at another place: /home -> <volume>/home */
int      vfs_alias(const char *path, const char *target);

/* mounts */
int      vfs_mount(const char *path, struct fs_mount *m, uint32_t root_ino);
int      vfs_umount(const char *path);
int      vfs_mount_generation(void);                   /* changes on every mount/umount */
struct fs_mount *vfs_mounts(void);
struct fs_mount *vfs_mount_of(const char *path);
int      vfs_sync(void);
vnode_t *vfs_fs_child(vnode_t *dir, const char *name, int type, size_t size, int64_t mtime, uint32_t ino);
void     vfs_start_syncer(void);
const char *vfs_basename(const char *path);
void     vfs_dirname(const char *path, char *out, size_t sz);

/* errors (negative return values) */
#define E_NOENT  -2
#define E_IO     -5
#define E_NOMEM  -12
#define E_EXIST  -17
#define E_NOTDIR -20
#define E_ISDIR  -21
#define E_INVAL  -22
#define E_NOTEMPTY -39

#endif

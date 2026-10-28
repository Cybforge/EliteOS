#pragma once
#include <stddef.h>
typedef struct { int v; } mutex_t;
#define MUTEX_INIT(n) { 0 }
static inline void mutex_lock(mutex_t *m) { (void)m; }
static inline void mutex_unlock(mutex_t *m) { (void)m; }
struct vfs_dirent { char name[64]; int type; size_t size; long mtime; };
#define VFS_PATH_MAX 256
int vfs_list(const char *p, struct vfs_dirent *d, int m);
char *vfs_read_file(const char *p, size_t *n);
struct fs_mount { struct fs_mount *next; char path[64]; };
struct fs_mount *vfs_mounts(void);

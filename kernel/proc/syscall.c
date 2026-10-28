/*
 * System call dispatch. User pointers are validated against the calling
 * process's page tables before the kernel touches them.
 */
#include <kernel.h>
#include <cpu.h>
#include <x86.h>
#include <sched.h>
#include <vfs.h>
#include <tty.h>
#include <wm.h>
#include <dev.h>
#include <syscall.h>
#include "file.h"

void proc_check_killed(struct regs *r);
void sv_syscall(struct regs *r);

void syscall_init_this(void)
{
    wrmsr(MSR_STAR, ((uint64_t)0x18 << 48) | ((uint64_t)GDT_KCODE << 32));
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
    wrmsr(MSR_SFMASK, RFLAGS_IF | (1u << 10) | (1u << 8) | (1u << 18));   /* IF DF TF AC */
}

#define EFAULT (-14)
#define EBADF  (-9)
#define ENOSYS (-38)
#define EINVAL (-22)
#define EMFILE (-24)

static process_t *cur(void) { return thread_current()->proc; }

static bool uok(const void *p, size_t n, bool w)
{
    return vmm_user_ok(cur()->as, (uint64_t)p, n, w);
}

/* copy a NUL-terminated user string into a kernel buffer */
static bool ustr(const char *u, char *out, size_t cap)
{
    for (size_t i = 0; i < cap; i++) {
        if (!uok(u + i, 1, false)) return false;
        out[i] = u[i];
        if (!out[i]) return true;
    }
    out[cap - 1] = 0;
    return true;
}

static void upath(process_t *p, const char *in, char *out)
{
    vfs_resolve(p->cwd, in, out, VFS_PATH_MAX);
}

static int alloc_fd(process_t *p, struct file *f)
{
    for (int i = 0; i < MAX_FDS; i++)
        if (!p->fds[i]) { p->fds[i] = f; return i; }
    return EMFILE;
}

static struct file *getf(process_t *p, int64_t fd)
{
    if (fd < 0 || fd >= MAX_FDS) return NULL;
    return p->fds[fd];
}

static long sys_write(process_t *p, int64_t fd, const char *buf, size_t n)
{
    struct file *f = getf(p, fd);
    if (!f) return EBADF;
    if (!uok(buf, n, false)) return EFAULT;
    switch (f->kind) {
    case F_TTY: tty_write(f->tty, buf, n); return (long)n;
    case F_LOG: {
        char tmp[256];
        size_t c = MIN(n, sizeof(tmp) - 1);
        memcpy(tmp, buf, c);
        tmp[c] = 0;
        if (c && tmp[c - 1] == '\n') tmp[c - 1] = 0;
        if (tmp[0]) klog("[%s] %s", p->name, tmp);
        return (long)n;
    }
    case F_VNODE: {
        if (f->flags & O_APPEND) f->off = f->vn->size;
        long r = vfs_write(f->vn, f->off, buf, n);
        if (r > 0) f->off += (uint64_t)r;
        return r;
    }
    default: return (long)n;
    }
}

static long sys_read(process_t *p, int64_t fd, char *buf, size_t n)
{
    struct file *f = getf(p, fd);
    if (!f) return EBADF;
    if (!uok(buf, n, true)) return EFAULT;
    switch (f->kind) {
    case F_TTY: {
        /* read through a kernel bounce buffer so a kill can interrupt us */
        char tmp[512];
        long r = tty_read(f->tty, tmp, MIN(n, sizeof(tmp)));
        if (r > 0) memcpy(buf, tmp, (size_t)r);
        return r;
    }
    case F_VNODE: {
        long r = vfs_read(f->vn, f->off, buf, n);
        if (r > 0) f->off += (uint64_t)r;
        return r;
    }
    default: return 0;
    }
}

static long sys_open(process_t *p, const char *upath_, int flags)
{
    char in[VFS_PATH_MAX], path[VFS_PATH_MAX];
    if (!ustr(upath_, in, sizeof(in))) return EFAULT;
    upath(p, in, path);
    vnode_t *vn = vfs_open(path, flags & O_CREAT);
    if (!vn) return E_NOENT;
    if (vn->type == VN_DIR) { vfs_close(vn); return E_ISDIR; }
    if (flags & O_TRUNC) vfs_truncate(vn, 0);
    struct file *f = kzalloc(sizeof(*f));
    f->kind = F_VNODE;
    f->vn = vn;
    f->refs = 1;
    f->flags = flags;
    int fd = alloc_fd(p, f);
    if (fd < 0) { vfs_close(vn); kfree(f); }
    return fd;
}

static long sys_sbrk(process_t *p, int64_t inc)
{
    uint64_t old = p->brk;
    if (inc < 0) return (long)old;           /* shrinking is a no-op */
    uint64_t want = old + (uint64_t)inc;
    if (want > USER_STACK_TOP - USER_STACK_SIZE - MiB(16) || want < old) return -12;
    for (uint64_t va = ALIGN_UP(old, PAGE_SIZE); va < want; va += PAGE_SIZE) {
        if (vmm_translate(p->as, va)) continue;
        uint64_t pa = pmm_alloc();
        if (!pa) return -12;
        vmm_map(p->as, va, pa, PTE_U | PTE_W | PTE_OWNED | (cpu_info.nx ? PTE_NX : 0));
        p->mem_pages++;
    }
    p->brk = want;
    return (long)old;
}

static long sys_spawn(process_t *p, const char *upath_, char **uargv)
{
    char in[VFS_PATH_MAX], path[VFS_PATH_MAX];
    if (!ustr(upath_, in, sizeof(in))) return EFAULT;
    if (!strchr(in, '/')) snprintf(path, sizeof(path), "/bin/%s", in);
    else upath(p, in, path);
    char *argv[16];
    char store[16][128];
    int argc = 0;
    if (uargv) {
        for (; argc < 15; argc++) {
            if (!uok(&uargv[argc], 8, false)) return EFAULT;
            if (!uargv[argc]) break;
            if (!ustr(uargv[argc], store[argc], sizeof(store[argc]))) return EFAULT;
            argv[argc] = store[argc];
        }
    }
    if (!argc) { strlcpy(store[0], path, sizeof(store[0])); argv[0] = store[0]; argc = 1; }
    argv[argc] = NULL;
    process_t *c = proc_spawn(path, argc, argv, p->tty, p->cwd);
    return c ? c->pid : E_NOENT;
}

static window_t *getwin(process_t *p, int64_t h)
{
    if (h < 0 || h >= p->nwindows) return NULL;
    return p->windows[h];
}

static long sys_win_create(process_t *p, int w, int h, const char *utitle)
{
    char title[64];
    if (!ustr(utitle, title, sizeof(title))) return EFAULT;
    if (w < 16 || h < 16 || w > 4096 || h > 4096) return EINVAL;
    int slot = -1;
    for (int i = 0; i < 8; i++) if (!p->windows[i]) { slot = i; break; }
    if (slot < 0) return EMFILE;
    window_t *win = wm_create(title, w, h, 0);
    if (!win) return -12;
    wm_set_icon(win, ICON_APP);
    win->proc = p;
    p->windows[slot] = win;
    if (slot >= p->nwindows) p->nwindows = slot + 1;
    return slot;
}

static long sys_win_present(process_t *p, int64_t h, const uint32_t *px)
{
    window_t *w = getwin(p, h);
    if (!w) return EBADF;
    surface_t *s = wm_begin(w);
    size_t bytes = (size_t)s->w * s->h * 4;
    if (!uok(px, bytes, false)) return EFAULT;
    for (int y = 0; y < s->h; y++) {
        uint32_t *d = s->px + (size_t)y * s->stride;
        const uint32_t *src = px + (size_t)y * s->w;
        for (int x = 0; x < s->w; x++) d[x] = src[x] | 0xFF000000u;
    }
    wm_present(w);
    return 0;
}

static long sys_win_event(process_t *p, int64_t h, struct z_event *uev, int timeout)
{
    window_t *w = getwin(p, h);
    if (!w) return EBADF;
    if (!uok(uev, sizeof(*uev), true)) return EFAULT;
    struct gui_event ev;
    /* wait in short slices so that kill() is noticed */
    int left = timeout;
    for (;;) {
        int slice = left < 0 ? 100 : MIN(left, 100);
        if (wm_wait_event(w, &ev, slice ? slice : 0)) break;
        if (p->killed) return 0;
        if (left >= 0) {
            left -= slice;
            if (left <= 0) return 0;
        }
    }
    uev->type = ev.type;
    uev->key = ev.key;
    uev->ch = ev.ch;
    uev->mods = ev.mods;
    uev->x = ev.x;
    uev->y = ev.y;
    uev->button = ev.button;
    uev->buttons = ev.buttons;
    uev->wheel = ev.wheel;
    return 1;
}

static long sys_draw_text(process_t *p, uint32_t *px, int64_t wh, int64_t xy, const char *utext, int64_t color_size)
{
    UNUSED(p);
    int w = (int)(wh >> 32), h = (int)(wh & 0xFFFFFFFF);
    int x = (int)(xy >> 32), y = (int)(int32_t)(xy & 0xFFFFFFFF);
    char text[256];
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return EINVAL;
    if (!ustr(utext, text, sizeof(text))) return EFAULT;
    if (!uok(px, (size_t)w * h * 4, true)) return EFAULT;
    surface_t s = surface_wrap(px, w, h, w);
    int size = (int)(color_size >> 32);
    font_t *f = size >= 30 ? font_light : size >= 18 ? font_title : size >= 15 ? font_ui_lg : size == 1 ? font_mono : font_ui;
    return gfx_text(&s, f, x, y, text, (uint32_t)color_size | 0xFF000000u) - x;
}

void syscall_dispatch(struct regs *r)
{
    process_t *p = cur();
    uint64_t a0 = r->rdi, a1 = r->rsi, a2 = r->rdx, a3 = r->r10, a4 = r->r8;
    UNUSED(a4);
    long ret = ENOSYS;
    if (!p) { r->rax = (uint64_t)ENOSYS; return; }
    if (p->sv) { sv_syscall(r); proc_check_killed(r); return; }       /* a program of the System V interface */

    switch (r->rax) {
    case SYS_EXIT:
        proc_exit((int)a0);
        break;
    case SYS_WRITE: ret = sys_write(p, (int64_t)a0, (const char *)a1, a2); break;
    case SYS_READ: ret = sys_read(p, (int64_t)a0, (char *)a1, a2); break;
    case SYS_OPEN: ret = sys_open(p, (const char *)a0, (int)a1); break;
    case SYS_CLOSE: {
        struct file *f = getf(p, (int64_t)a0);
        if (!f) { ret = EBADF; break; }
        p->fds[a0] = NULL;
        file_put(f);
        ret = 0;
        break;
    }
    case SYS_SBRK: ret = sys_sbrk(p, (int64_t)a0); break;
    case SYS_SLEEP: {
        uint64_t end = uptime_ms() + a0;
        while (uptime_ms() < end && !p->killed) sched_sleep(MIN(end - uptime_ms(), 100ul));
        ret = 0;
        break;
    }
    case SYS_UPTIME: ret = (long)uptime_ms(); break;
    case SYS_GETPID: ret = p->pid; break;
    case SYS_SPAWN: ret = sys_spawn(p, (const char *)a0, (char **)a1); break;
    case SYS_WAIT: ret = proc_wait((int)a0); break;
    case SYS_READDIR: {
        char in[VFS_PATH_MAX], path[VFS_PATH_MAX];
        struct z_dirent *ud = (struct z_dirent *)a2;
        if (!ustr((const char *)a0, in, sizeof(in)) || !uok(ud, sizeof(*ud), true)) { ret = EFAULT; break; }
        upath(p, in, path);
        struct vfs_dirent de;
        ret = vfs_readdir(path, (int)a1, &de);
        if (ret == 0) {
            strlcpy(ud->name, de.name, sizeof(ud->name));
            ud->type = de.type;
            ud->size = de.size;
        }
        break;
    }
    case SYS_WIN_CREATE: ret = sys_win_create(p, (int)a0, (int)a1, (const char *)a2); break;
    case SYS_WIN_PRESENT: ret = sys_win_present(p, (int64_t)a0, (const uint32_t *)a1); break;
    case SYS_WIN_EVENT: ret = sys_win_event(p, (int64_t)a0, (struct z_event *)a1, (int)a2); break;
    case SYS_WIN_CLOSE: {
        window_t *w = getwin(p, (int64_t)a0);
        if (!w) { ret = EBADF; break; }
        wm_destroy(w);
        p->windows[a0] = NULL;
        ret = 0;
        break;
    }
    case SYS_TIME: {
        struct z_time *t = (struct z_time *)a0;
        if (!uok(t, sizeof(*t), true)) { ret = EFAULT; break; }
        struct datetime dt;
        rtc_now(&dt);
        t->year = dt.year; t->month = dt.month; t->day = dt.day;
        t->hour = dt.hour; t->minute = dt.minute; t->second = dt.second; t->weekday = dt.weekday;
        ret = 0;
        break;
    }
    case SYS_SYSINFO: {
        struct z_sysinfo *si = (struct z_sysinfo *)a0;
        if (!uok(si, sizeof(*si), true)) { ret = EFAULT; break; }
        strlcpy(si->os, ZENITH_NAME, sizeof(si->os));
        strlcpy(si->version, ZENITH_VERSION, sizeof(si->version));
        strlcpy(si->cpu, cpu_info.brand, sizeof(si->cpu));
        si->ncpus = ncpus;
        si->mem_total = pmm_total_pages() * PAGE_SIZE;
        si->mem_free = pmm_free_pages() * PAGE_SIZE;
        si->uptime_ms = uptime_ms();
        si->threads = sched_thread_count();
        si->screen_w = wm_screen_w();
        si->screen_h = wm_screen_h();
        ret = 0;
        break;
    }
    case SYS_YIELD: sched_yield(); ret = 0; break;
    case SYS_SEEK: {
        struct file *f = getf(p, (int64_t)a0);
        if (!f || f->kind != F_VNODE) { ret = EBADF; break; }
        int64_t off = (int64_t)a1;
        if (a2 == 1) off += (int64_t)f->off;
        else if (a2 == 2) off += (int64_t)f->vn->size;
        if (off < 0) { ret = EINVAL; break; }
        f->off = (uint64_t)off;
        ret = off;
        break;
    }
    case SYS_STAT: {
        char in[VFS_PATH_MAX], path[VFS_PATH_MAX];
        struct z_stat *st = (struct z_stat *)a1;
        if (!ustr((const char *)a0, in, sizeof(in)) || !uok(st, sizeof(*st), true)) { ret = EFAULT; break; }
        upath(p, in, path);
        struct vfs_stat vs;
        ret = vfs_stat(path, &vs);
        if (!ret) { st->type = vs.type; st->size = vs.size; st->mtime = vs.mtime; }
        break;
    }
    case SYS_MKDIR: case SYS_UNLINK: case SYS_CHDIR: {
        char in[VFS_PATH_MAX], path[VFS_PATH_MAX];
        if (!ustr((const char *)a0, in, sizeof(in))) { ret = EFAULT; break; }
        upath(p, in, path);
        if (r->rax == SYS_MKDIR) ret = vfs_mkdir(path);
        else if (r->rax == SYS_UNLINK) ret = vfs_unlink(path, false);
        else {
            struct vfs_stat vs;
            ret = vfs_stat(path, &vs);
            if (!ret && vs.type != VN_DIR) ret = E_NOTDIR;
            if (!ret) strlcpy(p->cwd, path, sizeof(p->cwd));
        }
        break;
    }
    case SYS_GETCWD: {
        char *ub = (char *)a0;
        size_t n = a1;
        if (!n || !uok(ub, n, true)) { ret = EFAULT; break; }
        strlcpy(ub, p->cwd, n);
        ret = 0;
        break;
    }
    case SYS_RANDOM: ret = (long)(krand() ^ (rdtsc() & 0xFFFF)); break;
    case SYS_DRAW_TEXT: ret = sys_draw_text(p, (uint32_t *)a0, (int64_t)a1, (int64_t)a2, (const char *)a3, (int64_t)r->r8); break;
    case SYS_TTY_MODE:
        if (p->tty) { p->tty->raw = false; }
        ret = 0;
        break;
    }
    r->rax = (uint64_t)ret;
    proc_check_killed(r);
}

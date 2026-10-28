/*
 * The files programs of the System V interface expect to find: /proc (what
 * the system and each process look like), a little of /sys, /dev nodes, and
 * /etc files that are generated from the real configuration (accounts,
 * network, time zone) instead of being stored.
 */
#include <kernel.h>
#include <cpu.h>
#include <sched.h>
#include <mm.h>
#include <dev.h>
#include <sys.h>
#include <net.h>
#include <crypto.h>
#include <umm.h>
#include <abierr.h>
#include "sv.h"
#include "svcalls.h"
extern spinlock_t sv_lock;

/* ------------------------------------------------------------------------
 * text files
 * ---------------------------------------------------------------------- */

struct text { char *data; size_t len; };

static long text_read(struct sfile *f, void *buf, size_t n, uint64_t *off)
{
    struct text *t = f->priv;
    if (*off >= t->len) return 0;
    size_t c = MIN(n, t->len - (size_t)*off);
    memcpy(buf, t->data + *off, c);
    *off += c;
    return (long)c;
}

static long text_seek(struct sfile *f, int64_t off, int whence)
{
    struct text *t = f->priv;
    int64_t base = whence == 0 ? 0 : whence == 1 ? (int64_t)f->off : (int64_t)t->len;
    if (whence > 2 || base + off < 0) return -ERR_INVAL;
    f->off = (uint64_t)(base + off);
    return (long)f->off;
}

static int text_stat(struct sfile *f, struct vfs_stat *st, uint32_t *mode, uint64_t *ino)
{
    struct text *t = f->priv;
    st->type = VN_FILE;
    st->size = t->len;
    st->mtime = rtc_epoch();
    *mode = 0100444;
    *ino = (uint64_t)(uintptr_t)t >> 4;
    return 0;
}

static void text_close(struct sfile *f)
{
    struct text *t = f->priv;
    kfree(t->data);
    kfree(t);
}

static uint32_t text_poll(struct sfile *f, uint32_t ev) { UNUSED(f); return ev & 5; }

static const struct sfile_ops text_ops = {
    .read = text_read, .seek = text_seek, .stat = text_stat, .close = text_close, .poll = text_poll, .name = "proc",
};

/* takes ownership of `data` */
static struct sfile *text_file(char *data, size_t len, const char *path)
{
    struct sfile *f = sfile_new(&text_ops, 0);
    struct text *t = kzalloc(sizeof(*t));
    if (!f || !t) { kfree(data); kfree(t); sfile_put(f); return NULL; }
    t->data = data;
    t->len = len;
    f->priv = t;
    strlcpy(f->path, path, sizeof(f->path));
    return f;
}

/* ---- virtual directories ---- */

struct vdir { char (*names)[48]; uint8_t *types; int n; };

static long vdir_getdents(struct sfile *f, uint8_t *buf, size_t cap)
{
    struct vdir *d = f->priv;
    size_t o = 0;
    for (;;) {
        uint32_t idx = f->dirpos;
        const char *name;
        int type;
        if (idx == 0) { name = "."; type = 4; }
        else if (idx == 1) { name = ".."; type = 4; }
        else if ((int)idx - 2 < d->n) { name = d->names[idx - 2]; type = d->types[idx - 2]; }
        else break;
        size_t nl = strlen(name), reclen = ALIGN_UP(19 + nl + 1, 8);
        if (o + reclen > cap) { if (!o) return -ERR_INVAL; break; }
        memset(buf + o, 0, reclen);
        uint64_t ino = (uintptr_t)d / 8 + idx + 1, doff = idx + 1;
        memcpy(buf + o, &ino, 8);
        memcpy(buf + o + 8, &doff, 8);
        *(uint16_t *)(buf + o + 16) = (uint16_t)reclen;
        buf[o + 18] = (uint8_t)type;
        memcpy(buf + o + 19, name, nl + 1);
        o += reclen;
        f->dirpos++;
    }
    return (long)o;
}

static int vdir_stat(struct sfile *f, struct vfs_stat *st, uint32_t *mode, uint64_t *ino)
{
    st->type = VN_DIR;
    st->size = 0;
    st->mtime = rtc_epoch();
    *mode = 040555;
    *ino = (uint64_t)(uintptr_t)f->priv >> 4;
    return 0;
}

static void vdir_close(struct sfile *f)
{
    struct vdir *d = f->priv;
    kfree(d->names);
    kfree(d->types);
    kfree(d);
}

static long vdir_seek(struct sfile *f, int64_t off, int whence) { UNUSED(off); UNUSED(whence); f->dirpos = 0; return 0; }
static long vdir_read(struct sfile *f, void *b, size_t n, uint64_t *o) { UNUSED(f); UNUSED(b); UNUSED(n); UNUSED(o); return -ERR_ISDIR; }

static const struct sfile_ops vdir_ops = { .getdents = vdir_getdents, .stat = vdir_stat, .close = vdir_close, .seek = vdir_seek, .read = vdir_read, .name = "procdir" };

static struct sfile *vdir_new(const char *path, int max)
{
    struct sfile *f = sfile_new(&vdir_ops, 0);
    struct vdir *d = kzalloc(sizeof(*d));
    if (!f || !d) { kfree(d); sfile_put(f); return NULL; }
    d->names = kzalloc((size_t)max * 48);
    d->types = kzalloc((size_t)max);
    f->priv = d;
    strlcpy(f->path, path, sizeof(f->path));
    return f;
}

static void vdir_add(struct sfile *f, const char *name, int type)
{
    struct vdir *d = f->priv;
    strlcpy(d->names[d->n], name, 48);
    d->types[d->n++] = (uint8_t)type;
}

/* ------------------------------------------------------------------------
 * a string builder
 * ---------------------------------------------------------------------- */

struct sb { char *p; size_t n, cap; };

static void sb_init(struct sb *b, size_t cap) { b->p = kmalloc(cap); b->n = 0; b->cap = cap; if (b->p) b->p[0] = 0; }
static void sb_printf(struct sb *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void sb_printf(struct sb *b, const char *fmt, ...)
{
    if (!b->p || b->n + 1 >= b->cap) return;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(b->p + b->n, b->cap - b->n, fmt, ap);
    va_end(ap);
    if (w > 0) b->n = MIN(b->n + (size_t)w, b->cap - 1);
}

/* ------------------------------------------------------------------------
 * /proc/<pid>/...
 * ---------------------------------------------------------------------- */

static void fill_status(struct sb *b, struct sv_group *g, struct sv_task *t)
{
    struct mm *m = g->mm;
    sb_printf(b, "Name:\t%s\nUmask:\t%04o\nState:\t%s\nTgid:\t%d\nNgid:\t0\nPid:\t%d\nPPid:\t%d\nTracerPid:\t0\n", g->comm, g->umask,
              g->zombie ? "Z (zombie)" : "S (sleeping)", g->tgid, t ? t->tid : g->tgid, g->parent ? g->parent->tgid : g->ppid);
    sb_printf(b, "Uid:\t%u\t%u\t%u\t%u\nGid:\t%u\t%u\t%u\t%u\nFDSize:\t%d\n", g->uid, g->euid, g->uid, g->uid, g->gid, g->egid, g->gid,
              g->gid, g->files ? g->files->cap : 0);
    if (m) sb_printf(b, "VmPeak:\t%lu kB\nVmSize:\t%lu kB\nVmRSS:\t%lu kB\nVmData:\t%lu kB\nVmStk:\t132 kB\n", m->vm_pages * 4, m->vm_pages * 4,
                     m->rss_pages * 4, m->vm_pages * 4);
    sb_printf(b, "Threads:\t%d\nSigQ:\t0/1000\nSigPnd:\t%016lx\nSigBlk:\t%016lx\nSigIgn:\t0000000000000000\nSigCgt:\t0000000000000000\n",
              g->nthreads, t ? t->pending : 0, t ? t->blocked : 0);
    sb_printf(b, "CapInh:\t0000000000000000\nCapPrm:\t0000000000000000\nCapEff:\t0000000000000000\nCapBnd:\t000001ffffffffff\n");
    sb_printf(b, "NoNewPrivs:\t0\nSeccomp:\t0\nCpus_allowed:\tff\nCpus_allowed_list:\t0-%d\nvoluntary_ctxt_switches:\t1\nnonvoluntary_ctxt_switches:\t0\n", ncpus - 1);
}

static void fill_stat(struct sb *b, struct sv_group *g, struct sv_task *t)
{
    struct mm *m = g->mm;
    uint64_t ut = 0;
    for (struct sv_task *x = g->tasks; x; x = x->next) if (x->thr) ut += x->thr->ticks;
    sb_printf(b, "%d (%s) %c %d %d %d 0 -1 0 0 0 0 0 %lu 0 0 0 20 0 %d 0 %lu %lu %lu 18446744073709551615 4194304 4194304 %lu 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0 %lu %lu %lu %lu %lu %lu %lu 0\n",
              g->tgid, g->comm, g->zombie ? 'Z' : 'S', g->parent ? g->parent->tgid : g->ppid, g->pgid, g->sid, ut / 10, g->nthreads,
              g->start_ms / 10, m ? m->vm_pages * PAGE_SIZE : 0, m ? m->rss_pages : 0, m ? m->stack_top : 0, m ? m->brk_start : 0,
              m ? m->brk : 0, g->arg_start, g->arg_end, g->env_start, g->env_end, (unsigned long)g->exit_code);
    UNUSED(t);
}

static char *read_user_area(struct sv_group *g, uint64_t start, uint64_t end, size_t *len)
{
    *len = 0;
    if (!g->mm || end <= start || end - start > (1 << 20)) return kzalloc(1);
    size_t n = (size_t)(end - start);
    char *d = kmalloc(n + 1);
    if (!d) return NULL;
    if (mm_access(g->mm, start, d, n, false)) { kfree(d); return kzalloc(1); }
    d[n] = 0;
    *len = n;
    return d;
}

static struct sfile *proc_pid_file(struct sv_group *g, struct sv_task *t, const char *rest, const char *full)
{
    struct sb b;
    sb_init(&b, 16384);
    if (!b.p) return NULL;
    if (!strcmp(rest, "maps") || !strcmp(rest, "smaps")) { if (g->mm) b.n = mm_maps(g->mm, b.p, b.cap); }
    else if (!strcmp(rest, "status")) fill_status(&b, g, t);
    else if (!strcmp(rest, "stat")) fill_stat(&b, g, t);
    else if (!strcmp(rest, "statm")) sb_printf(&b, "%lu %lu 0 0 0 %lu 0\n", g->mm ? g->mm->vm_pages : 0, g->mm ? g->mm->rss_pages : 0, g->mm ? g->mm->vm_pages : 0);
    else if (!strcmp(rest, "comm")) sb_printf(&b, "%s\n", g->comm);
    else if (!strcmp(rest, "cmdline")) {
        size_t l;
        char *d = read_user_area(g, g->arg_start, g->arg_end, &l);
        kfree(b.p);
        return d ? text_file(d, l, full) : NULL;
    } else if (!strcmp(rest, "environ")) {
        size_t l;
        char *d = read_user_area(g, g->env_start, g->env_end, &l);
        kfree(b.p);
        return d ? text_file(d, l, full) : NULL;
    } else if (!strcmp(rest, "limits")) {
        static const char *names[] = { "Max cpu time", "Max file size", "Max data size", "Max stack size", "Max core file size",
                                       "Max resident set", "Max processes", "Max open files", "Max locked memory", "Max address space" };
        sb_printf(&b, "%-25s %-20s %-20s %-10s\n", "Limit", "Soft Limit", "Hard Limit", "Units");
        for (int i = 0; i < 10; i++) {
            char s[24], h[24];
            if (g->rlim_cur[i] == ~0ull) strlcpy(s, "unlimited", sizeof(s)); else snprintf(s, sizeof(s), "%lu", g->rlim_cur[i]);
            if (g->rlim_max[i] == ~0ull) strlcpy(h, "unlimited", sizeof(h)); else snprintf(h, sizeof(h), "%lu", g->rlim_max[i]);
            sb_printf(&b, "%-25s %-20s %-20s\n", names[i], s, h);
        }
    } else if (!strcmp(rest, "mountinfo") || !strcmp(rest, "mounts")) {
        sb_printf(&b, "%s", "ramfs / ramfs rw 0 0\n");
        int id = 30;
        for (struct fs_mount *m = vfs_mounts(); m; m = m->next, id++) {
            if (!strcmp(rest, "mounts")) sb_printf(&b, "/dev/%s %s %s rw 0 0\n", m->dev, m->path, m->fstype);
            else sb_printf(&b, "%d 1 0:%d / %s rw - %s /dev/%s rw\n", id, id, m->path, m->fstype, m->dev);
        }
    } else if (!strcmp(rest, "cgroup")) sb_printf(&b, "0::/\n");
    else if (!strcmp(rest, "oom_score_adj") || !strcmp(rest, "oom_adj")) sb_printf(&b, "0\n");
    else if (!strcmp(rest, "oom_score")) sb_printf(&b, "0\n");
    else if (!strcmp(rest, "loginuid")) sb_printf(&b, "%u\n", g->uid);
    else if (!strcmp(rest, "auxv")) { /* empty */ }
    else if (!strcmp(rest, "wchan")) sb_printf(&b, "0");
    else if (!strcmp(rest, "personality")) sb_printf(&b, "00000000\n");
    else { kfree(b.p); return NULL; }
    return text_file(b.p, b.n, full);
}

static struct sfile *proc_pid_dir(struct sv_group *g, const char *rest, const char *full)
{
    if (!*rest) {
        struct sfile *d = vdir_new(full, 40);
        static const char *files[] = { "maps", "smaps", "status", "stat", "statm", "comm", "cmdline", "environ", "limits", "mountinfo",
                                       "mounts", "cgroup", "oom_score_adj", "auxv" };
        for (unsigned i = 0; i < ARRAY_SIZE(files); i++) vdir_add(d, files[i], 8);
        vdir_add(d, "fd", 4);
        vdir_add(d, "task", 4);
        vdir_add(d, "exe", 10);
        vdir_add(d, "cwd", 10);
        return d;
    }
    if (!strcmp(rest, "fd")) {
        struct sfile *d = vdir_new(full, 1100);
        mutex_lock(&g->files->lock);
        for (int i = 0; i < g->files->cap && d && ((struct vdir *)d->priv)->n < 1090; i++)
            if (g->files->fd[i]) { char n[12]; snprintf(n, sizeof(n), "%d", i); vdir_add(d, n, 10); }
        mutex_unlock(&g->files->lock);
        return d;
    }
    if (!strcmp(rest, "task")) {
        struct sfile *d = vdir_new(full, 64);
        spin_lock(&sv_lock);
        for (struct sv_task *t = g->tasks; t && ((struct vdir *)d->priv)->n < 60; t = t->next) { char n[12]; snprintf(n, sizeof(n), "%d", t->tid); vdir_add(d, n, 4); }
        spin_unlock(&sv_lock);
        return d;
    }
    return NULL;
}

/* ------------------------------------------------------------------------
 * system-wide /proc files
 * ---------------------------------------------------------------------- */

static void fill_cpuinfo(struct sb *b)
{
    for (int i = 0; i < ncpus; i++) {
        sb_printf(b, "processor\t: %d\nvendor_id\t: %s\ncpu family\t: %u\nmodel\t\t: %u\nmodel name\t: %s\nstepping\t: %u\n", i, cpu_info.vendor,
                  cpu_info.family, cpu_info.model, cpu_info.brand, cpu_info.stepping);
        sb_printf(b, "cpu MHz\t\t: %lu.000\ncache size\t: 1024 KB\nphysical id\t: 0\nsiblings\t: %d\ncore id\t\t: %d\ncpu cores\t: %d\n", tsc_hz() / 1000000, ncpus, i, ncpus);
        sb_printf(b, "fpu\t\t: yes\nfpu_exception\t: yes\ncpuid level\t: 13\nwp\t\t: yes\nflags\t\t: fpu de pse tsc msr pae cx8 apic sep mtrr pge cmov pat pse36 clflush mmx fxsr sse sse2 syscall nx lm constant_tsc rep_good nopl cpuid pni%s%s%s%s%s%s\n",
                  cpu_info.ssse3 ? " ssse3" : "", cpu_info.sse41 ? " sse4_1" : "", cpu_info.sse42 ? " sse4_2" : "", cpu_info.aes ? " aes" : "",
                  cpu_info.rdrand ? " rdrand" : "", cpu_info.hypervisor ? " hypervisor" : "");
        sb_printf(b, "bogomips\t: %lu.00\nclflush size\t: 64\ncache_alignment\t: 64\naddress sizes\t: 48 bits physical, 48 bits virtual\n\n", tsc_hz() / 500000);
    }
}

static struct sfile *proc_global(const char *rest, const char *full)
{
    struct sb b;
    sb_init(&b, 16384);
    if (!b.p) return NULL;
    uint64_t total = pmm_total_pages() * 4, freek = pmm_free_pages() * 4;
    if (!strcmp(rest, "cpuinfo")) fill_cpuinfo(&b);
    else if (!strcmp(rest, "meminfo"))
        sb_printf(&b, "MemTotal:       %8lu kB\nMemFree:        %8lu kB\nMemAvailable:   %8lu kB\nBuffers:               0 kB\nCached:                0 kB\nSwapCached:            0 kB\nSwapTotal:             0 kB\nSwapFree:              0 kB\nCommitLimit:    %8lu kB\nVmallocTotal:   34359738367 kB\nHugePages_Total:       0\nHugepagesize:       2048 kB\n",
                  total, freek, freek, total);
    else if (!strcmp(rest, "version")) sb_printf(&b, "Linux version 6.1.0-zenith (zenith@zenithos) (ZenithOS %s) #1 SMP\n", ZENITH_VERSION);
    else if (!strcmp(rest, "uptime")) sb_printf(&b, "%lu.%02lu %lu.00\n", uptime_ms() / 1000, uptime_ms() / 10 % 100, uptime_ms() / 1000 * (unsigned long)ncpus);
    else if (!strcmp(rest, "loadavg")) sb_printf(&b, "0.10 0.10 0.10 1/%d %d\n", sched_thread_count(), 1000);
    else if (!strcmp(rest, "stat")) {
        sb_printf(&b, "cpu  %lu 0 0 %lu 0 0 0 0 0 0\n", uptime_ms() / 10, uptime_ms() / 10);
        for (int i = 0; i < ncpus; i++) sb_printf(&b, "cpu%d %lu 0 0 %lu 0 0 0 0 0 0\n", i, uptime_ms() / 10 / (unsigned long)ncpus, uptime_ms() / 10 / (unsigned long)ncpus);
        sb_printf(&b, "ctxt %lu\nbtime %ld\nprocesses 1\nprocs_running 1\nprocs_blocked 0\n", sched_context_switches(), (long)(rtc_epoch() - (int64_t)(uptime_ms() / 1000)));
    }
    else if (!strcmp(rest, "filesystems")) sb_printf(&b, "nodev\tramfs\nnodev\ttmpfs\nnodev\tproc\n\tvfat\n");
    else if (!strcmp(rest, "mounts") || !strcmp(rest, "self/mounts")) {
        sb_printf(&b, "ramfs / ramfs rw 0 0\nproc /proc proc rw 0 0\n");
        for (struct fs_mount *m = vfs_mounts(); m; m = m->next) sb_printf(&b, "/dev/%s %s %s rw 0 0\n", m->dev, m->path, m->fstype);
    }
    else if (!strcmp(rest, "cmdline")) sb_printf(&b, "BOOT_IMAGE=zenith\n");
    else if (!strcmp(rest, "sys/kernel/pid_max")) sb_printf(&b, "4194304\n");
    else if (!strcmp(rest, "sys/kernel/hostname")) sb_printf(&b, "%s\n", sys_hostname());
    else if (!strcmp(rest, "sys/kernel/osrelease")) sb_printf(&b, "6.1.0-zenith\n");
    else if (!strcmp(rest, "sys/kernel/ostype")) sb_printf(&b, "Linux\n");
    else if (!strcmp(rest, "sys/kernel/threads-max")) sb_printf(&b, "63000\n");
    else if (!strcmp(rest, "sys/kernel/random/boot_id")) sb_printf(&b, "6a7e2d9c-5b61-4f0e-8c2a-%012lx\n", (unsigned long)tsc_hz());
    else if (!strcmp(rest, "sys/kernel/random/uuid")) { uint8_t u[16]; random_bytes(u, 16); sb_printf(&b, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x\n", u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]); }
    else if (!strcmp(rest, "sys/vm/overcommit_memory")) sb_printf(&b, "1\n");
    else if (!strcmp(rest, "sys/vm/max_map_count")) sb_printf(&b, "1048576\n");
    else if (!strcmp(rest, "sys/vm/swappiness")) sb_printf(&b, "0\n");
    else if (!strcmp(rest, "sys/net/core/somaxconn")) sb_printf(&b, "4096\n");
    else if (!strcmp(rest, "sys/fs/file-max")) sb_printf(&b, "1048576\n");
    else if (!strcmp(rest, "sys/fs/inotify/max_user_watches")) sb_printf(&b, "65536\n");
    else if (!strcmp(rest, "sys/fs/inotify/max_user_instances")) sb_printf(&b, "128\n");
    else if (!strcmp(rest, "sys/fs/pipe-max-size")) sb_printf(&b, "1048576\n");
    else { kfree(b.p); return NULL; }
    return text_file(b.p, b.n, full);
}

static struct sfile *proc_open(struct sv_group *self, const char *rest, const char *full, int *err)
{
    *err = 0;
    if (!*rest || !strcmp(rest, "/")) {
        struct sfile *d = vdir_new(full, 600);
        static const char *files[] = { "cpuinfo", "meminfo", "version", "uptime", "loadavg", "stat", "filesystems", "mounts", "cmdline" };
        for (unsigned i = 0; i < ARRAY_SIZE(files); i++) vdir_add(d, files[i], 8);
        vdir_add(d, "self", 10);
        vdir_add(d, "sys", 4);
        int ids[256];
        int n = sv_group_list(ids, 256);
        for (int i = 0; i < n; i++) { char nm[12]; snprintf(nm, sizeof(nm), "%d", ids[i]); vdir_add(d, nm, 4); }
        return d;
    }
    rest++;                                              /* skip the slash */
    struct sv_group *g = NULL;
    struct sv_task *t = NULL;
    const char *tail = NULL;
    if (!strncmp(rest, "self", 4) && (rest[4] == 0 || rest[4] == '/')) { g = self; tail = rest + 4; }
    else if (!strncmp(rest, "thread-self", 11) && (rest[11] == 0 || rest[11] == '/')) { g = self; t = sv_cur(); tail = rest + 11; }
    else if (isdigit((unsigned char)rest[0])) {
        char *end;
        long pid = strtol(rest, &end, 10);
        g = sv_find_group((int)pid);
        if (!g) { *err = -ERR_NOENT; return NULL; }
        tail = end;
    }
    if (g) {
        if (*tail == '/') tail++;
        if (!t) t = g->tasks;
        /* /proc/<pid>/task/<tid>/... shares the thread group's files */
        if (!strncmp(tail, "task/", 5) && isdigit((unsigned char)tail[5])) {
            char *e2;
            long tid = strtol(tail + 5, &e2, 10);
            struct sv_task *tt = sv_find_task((int)tid);
            if (tt && tt->g == g) t = tt;
            tail = *e2 == '/' ? e2 + 1 : e2;
        }
        struct sfile *f = proc_pid_dir(g, tail, full);
        if (f) return f;
        if (!strncmp(tail, "fd/", 3)) {
            struct sfile *src = sv_fd_get(g, atoi(tail + 3));
            if (!src) { *err = -ERR_NOENT; return NULL; }
            return src;                                  /* opening /proc/self/fd/N gives the same file */
        }
        f = proc_pid_file(g, t, tail, full);
        if (!f) *err = -ERR_NOENT;
        return f;
    }
    struct sfile *f = proc_global(rest, full);
    if (f) return f;
    if (!strcmp(rest, "sys") || !strcmp(rest, "sys/kernel") || !strcmp(rest, "sys/vm") || !strcmp(rest, "sys/net") || !strcmp(rest, "sys/fs") ||
        !strcmp(rest, "sys/kernel/random")) {
        struct sfile *d = vdir_new(full, 16);
        return d;
    }
    *err = -ERR_NOENT;
    return NULL;
}

/* ------------------------------------------------------------------------
 * /sys
 * ---------------------------------------------------------------------- */

static struct sfile *sys_open(const char *rest, const char *full, int *err)
{
    *err = 0;
    struct sb b;
    sb_init(&b, 256);
    if (!b.p) { *err = -ERR_NOMEM; return NULL; }
    if (!strcmp(rest, "/devices/system/cpu/online") || !strcmp(rest, "/devices/system/cpu/present") || !strcmp(rest, "/devices/system/cpu/possible"))
        sb_printf(&b, "0-%d\n", ncpus - 1);
    else if (!strcmp(rest, "/devices/system/cpu/kernel_max")) sb_printf(&b, "%d\n", MAX_CPUS - 1);
    else if (!strcmp(rest, "/kernel/mm/transparent_hugepage/enabled")) sb_printf(&b, "always madvise [never]\n");
    else if (!strcmp(rest, "/kernel/mm/transparent_hugepage/hpage_pmd_size")) sb_printf(&b, "2097152\n");
    else if (!strcmp(rest, "/kernel/mm/transparent_hugepage/defrag")) sb_printf(&b, "always defer defer+madvise madvise [never]\n");
    else if (!strcmp(rest, "") || !strcmp(rest, "/") || !strcmp(rest, "/devices") || !strcmp(rest, "/devices/system") || !strcmp(rest, "/devices/system/cpu") ||
             !strcmp(rest, "/kernel") || !strcmp(rest, "/class") || !strcmp(rest, "/fs") || !strcmp(rest, "/kernel/mm") || !strcmp(rest, "/kernel/mm/transparent_hugepage")) {
        kfree(b.p);
        return vdir_new(full, 8);
    } else {
        kfree(b.p);
        *err = -ERR_NOENT;
        return NULL;
    }
    return text_file(b.p, b.n, full);
}

/* ------------------------------------------------------------------------
 * /etc files made from the real configuration
 * ---------------------------------------------------------------------- */

static struct sfile *etc_generated(const char *path, int *err)
{
    *err = 0;
    struct sb b;
    sb_init(&b, 4096);
    if (!b.p) return NULL;
    if (!strcmp(path, "/etc/passwd")) {
        sb_printf(&b, "root:x:0:0:root:/root:/bin/sh\n");
        for (int i = 0; i < users_count(); i++) {
            const struct account *a = users_get(i);
            sb_printf(&b, "%s:x:%d:%d:%s:/home/%s:/bin/sh\n", a->name, a->uid, a->uid, a->fullname, a->name);
        }
        sb_printf(&b, "nobody:x:65534:65534:nobody:/:/bin/false\n");
    } else if (!strcmp(path, "/etc/group")) {
        sb_printf(&b, "root:x:0:\n");
        for (int i = 0; i < users_count(); i++) sb_printf(&b, "%s:x:%d:%s\n", users_get(i)->name, users_get(i)->uid, users_get(i)->name);
        sb_printf(&b, "nogroup:x:65534:\n");
    } else if (!strcmp(path, "/etc/hosts")) {
        sb_printf(&b, "127.0.0.1\tlocalhost %s\n::1\t\tlocalhost ip6-localhost\n", sys_hostname());
    } else if (!strcmp(path, "/etc/resolv.conf")) {
        struct net_info ni;
        char ip[20];
        if (net_get_info(&ni) && ni.dns) { ip_to_str(ni.dns, ip); sb_printf(&b, "nameserver %s\n", ip); }
        else sb_printf(&b, "# no network yet\n");
    } else if (!strcmp(path, "/etc/nsswitch.conf")) {
        sb_printf(&b, "passwd: files\ngroup: files\nhosts: files dns\n");
    } else if (!strcmp(path, "/etc/host.conf")) {
        sb_printf(&b, "multi on\n");
    } else if (!strcmp(path, "/etc/os-release") || !strcmp(path, "/usr/lib/os-release")) {
        sb_printf(&b, "NAME=\"ZenithOS\"\nID=zenith\nVERSION=\"%s (%s)\"\nVERSION_ID=\"%s\"\nPRETTY_NAME=\"ZenithOS %s\"\n", ZENITH_VERSION, ZENITH_CODENAME, ZENITH_VERSION, ZENITH_VERSION);
    } else if (!strcmp(path, "/etc/shells")) {
        sb_printf(&b, "/bin/sh\n");
    } else if (!strcmp(path, "/etc/localtime")) {
        kfree(b.p);
        char zp[160];
        snprintf(zp, sizeof(zp), "/usr/share/zoneinfo/%s", tz_name());
        size_t n;
        char *d = vfs_read_file(zp, &n);
        if (!d) { *err = -ERR_NOENT; return NULL; }
        return text_file(d, n, path);
    } else if (!strcmp(path, "/etc/machine-id")) {
        size_t n;
        char *d = vfs_read_file("/etc/machine-id.store", &n);
        if (!d) {
            uint8_t u[16];
            random_bytes(u, 16);
            char id[40];
            for (int i = 0; i < 16; i++) snprintf(id + i * 2, 3, "%02x", u[i]);
            id[32] = '\n';
            sys_write_etc("machine-id.store", id, 33);
            d = vfs_read_file("/etc/machine-id.store", &n);
        }
        kfree(b.p);
        return d ? text_file(d, n, path) : NULL;
    } else {
        kfree(b.p);
        return NULL;
    }
    return text_file(b.p, b.n, path);
}

/* ------------------------------------------------------------------------
 * the hook the file calls use
 * ---------------------------------------------------------------------- */

/* 0: handled (*out is the file), 1: a normal path, <0: an error for a special path */
int sv_proc_path(struct sv_group *g, const char *path, struct sfile **out)
{
    *out = NULL;
    int err = 0;
    if (!strncmp(path, "/proc", 5) && (path[5] == 0 || path[5] == '/')) {
        struct sfile *f = proc_open(g, path + 5, path, &err);
        if (!f) return err ? err : -ERR_NOENT;
        *out = f;
        return 0;
    }
    if (!strncmp(path, "/sys", 4) && (path[4] == 0 || path[4] == '/')) {
        struct sfile *f = sys_open(path + 4, path, &err);
        if (!f) return err ? err : -ERR_NOENT;
        *out = f;
        return 0;
    }
    if (!strncmp(path, "/dev/fd/", 8) || !strcmp(path, "/dev/stdin") || !strcmp(path, "/dev/stdout") || !strcmp(path, "/dev/stderr")) {
        int fd = !strncmp(path, "/dev/fd/", 8) ? atoi(path + 8) : !strcmp(path, "/dev/stdin") ? 0 : !strcmp(path, "/dev/stdout") ? 1 : 2;
        struct sfile *f = sv_fd_get(g, fd);
        if (!f) return -ERR_NOENT;
        *out = f;
        return 0;
    }
    if (!strncmp(path, "/etc/", 5) || !strncmp(path, "/usr/lib/os-release", 19)) {
        struct sfile *f = etc_generated(path, &err);
        if (f) { *out = f; return 0; }
        if (err) return err;
    }
    return 1;
}

/* ------------------------------------------------------------------------
 * device nodes
 * ---------------------------------------------------------------------- */

static long dev_null_read(vnode_t *vn, void *b, size_t n, uint64_t off) { UNUSED(vn); UNUSED(b); UNUSED(n); UNUSED(off); return 0; }
static long dev_sink_write(vnode_t *vn, const void *b, size_t n, uint64_t off) { UNUSED(vn); UNUSED(b); UNUSED(off); return (long)n; }
static long dev_zero_read(vnode_t *vn, void *b, size_t n, uint64_t off) { UNUSED(vn); UNUSED(off); memset(b, 0, n); return (long)n; }
static long dev_full_write(vnode_t *vn, const void *b, size_t n, uint64_t off) { UNUSED(vn); UNUSED(b); UNUSED(n); UNUSED(off); return -ERR_NOSPC; }
static long dev_rand_read(vnode_t *vn, void *b, size_t n, uint64_t off) { UNUSED(vn); UNUSED(off); random_bytes(b, n); return (long)n; }

void sv_abi_init(void)
{
    sys_mkdir_p("/dev");
    sys_mkdir_p("/dev/shm");
    sys_mkdir_p("/tmp");
    sys_mkdir_p("/run");
    sys_mkdir_p("/proc");
    sys_mkdir_p("/sys");
    sys_mkdir_p("/var/tmp");
    vfs_register_dev("/dev/null", dev_null_read, dev_sink_write, NULL);
    vfs_register_dev("/dev/zero", dev_zero_read, dev_sink_write, NULL);
    vfs_register_dev("/dev/full", dev_zero_read, dev_full_write, NULL);
    vfs_register_dev("/dev/random", dev_rand_read, dev_sink_write, NULL);
    vfs_register_dev("/dev/urandom", dev_rand_read, dev_sink_write, NULL);
    vfs_register_dev("/dev/tty", dev_null_read, dev_sink_write, NULL);
    vfs_register_dev("/dev/console", dev_null_read, dev_sink_write, NULL);
}

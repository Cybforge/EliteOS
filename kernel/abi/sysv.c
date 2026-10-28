/*
 * The system call interface of the System V x86-64 program interface: the
 * dispatcher, memory, process, signal, time and information calls. File and
 * socket calls are in fsys.c and sock.c.
 */
#include <kernel.h>
#include <cpu.h>
#include <x86.h>
#include <sched.h>
#include <mm.h>
#include <dev.h>
#include <sys.h>
#include <umm.h>
#include <crypto.h>
#include <net.h>
#include <abierr.h>
#include "sv.h"
#include "svcalls.h"

#define A0 (r->rdi)
#define A1 (r->rsi)
#define A2 (r->rdx)
#define A3 (r->r10)
#define A4 (r->r8)
#define A5 (r->r9)

extern spinlock_t sv_lock;

/* ------------------------------------------------------------------------
 * waiting
 * ---------------------------------------------------------------------- */

/* Sleep on `chan` with `lk` held (it is held again on return). 0: woken, 1: timeout, -ERR_INTR: a signal is waiting. */
int sv_wait_chan(void *chan, spinlock_t *lk, uint64_t timeout_ms)
{
    struct sv_task *t = sv_cur();
    thread_t *th = thread_current();
    th->interrupted = 0;
    if (t && sv_signal_pending(t)) return -ERR_INTR;
    int r = sched_wait(chan, lk, timeout_ms);
    if (t && sv_signal_pending(t)) return -ERR_INTR;
    th->interrupted = 0;
    return r;
}

/* sleep for `ms` (or until a signal) with no object to wait on */
long sv_sleep_ms(uint64_t ms)
{
    static char chan;
    struct sv_task *t = sv_cur();
    thread_current()->interrupted = 0;
    if (sv_signal_pending(t)) return -ERR_INTR;
    sched_wait(&chan, NULL, ms ? ms : 1);
    if (sv_signal_pending(t)) return -ERR_INTR;
    return 0;
}

static spinlock_t poll_lock = SPINLOCK_INIT("svpoll");
static volatile uint64_t poll_gen;

void sv_poll_wake(void)
{
    __atomic_add_fetch(&poll_gen, 1, __ATOMIC_ACQ_REL);
    sched_wake((void *)&poll_gen);
}

uint64_t sv_poll_gen(void) { return __atomic_load_n(&poll_gen, __ATOMIC_ACQUIRE); }

/* Sleep until something changed after `gen` was read (read it before checking what you wait for), or the timeout
 * (0 = none): 0 woken, 1 timeout, -ERR_INTR a signal is waiting. */
long sv_poll_wait_gen(uint64_t gen, uint64_t timeout_ms)
{
    spin_lock(&poll_lock);
    long r = 0;
    while (poll_gen == gen) {
        int w = sv_wait_chan((void *)&poll_gen, &poll_lock, timeout_ms);
        if (w < 0) { r = w; break; }
        if (w == 1) { r = 1; break; }
    }
    spin_unlock(&poll_lock);
    return r;
}

/* ------------------------------------------------------------------------
 * time
 * ---------------------------------------------------------------------- */

uint64_t sv_now_ns(void)
{
    uint64_t hz = tsc_hz();
    if (!hz) return uptime_ms() * 1000000ull;
    uint64_t t = rdtsc();
    return (t / hz) * 1000000000ull + (t % hz) * 1000000000ull / hz;           /* no 128-bit division in the kernel */
}

uint64_t sv_realtime_ns(void)
{
    static int64_t offset;
    static bool have;
    uint64_t mono = sv_now_ns();
    int64_t wall_s = rtc_epoch();
    int64_t est = (int64_t)(mono) + offset;
    /* the second-resolution clock is the reference; keep the sub-second part from the monotonic clock */
    if (!have || est < wall_s * 1000000000ll - 1500000000ll || est > wall_s * 1000000000ll + 1500000000ll) {
        offset = wall_s * 1000000000ll - (int64_t)mono;
        have = true;
        est = wall_s * 1000000000ll;
    }
    return (uint64_t)est;
}

struct ts { int64_t sec, nsec; };

static void ns_to_ts(uint64_t ns, struct ts *t) { t->sec = (int64_t)(ns / 1000000000ull); t->nsec = (int64_t)(ns % 1000000000ull); }

static uint64_t cpu_time_ns(struct sv_group *g)
{
    uint64_t ms = 0;
    for (struct sv_task *t = g->tasks; t; t = t->next) if (t->thr) ms += t->thr->ticks;
    return ms * 1000000ull;
}

static long clock_now(int clk, struct ts *t)
{
    switch (clk) {
    case 0: case 5: case 8: ns_to_ts(sv_realtime_ns(), t); return 0;                 /* REALTIME */
    case 1: case 4: case 6: case 7: case 9: ns_to_ts(sv_now_ns(), t); return 0;        /* MONOTONIC, BOOTTIME ... */
    case 2: ns_to_ts(cpu_time_ns(sv_curgrp()), t); return 0;                          /* PROCESS_CPUTIME */
    case 3: ns_to_ts((thread_current()->ticks) * 1000000ull, t); return 0;            /* THREAD_CPUTIME */
    default: return -ERR_INVAL;
    }
}

static long s_clock_gettime(struct regs *r)
{
    struct ts t;
    long rc = clock_now((int)A0, &t);
    if (rc) return rc;
    return copy_to_user(A1, &t, sizeof(t)) ? -ERR_FAULT : 0;
}

static long s_clock_getres(struct regs *r)
{
    struct ts t = { 0, 1 };
    if (A1 && copy_to_user(A1, &t, sizeof(t))) return -ERR_FAULT;
    return 0;
}

static long s_clock_settime(struct regs *r) { UNUSED(r); return -ERR_PERM; }

static long s_gettimeofday(struct regs *r)
{
    if (A0) {
        uint64_t ns = sv_realtime_ns();
        int64_t tv[2] = { (int64_t)(ns / 1000000000ull), (int64_t)(ns % 1000000000ull / 1000) };
        if (copy_to_user(A0, tv, sizeof(tv))) return -ERR_FAULT;
    }
    if (A1) {
        int32_t tz[2] = { 0, 0 };
        if (copy_to_user(A1, tz, sizeof(tz))) return -ERR_FAULT;
    }
    return 0;
}

static long s_time(struct regs *r)
{
    int64_t t = (int64_t)(sv_realtime_ns() / 1000000000ull);
    if (A0 && copy_to_user(A0, &t, 8)) return -ERR_FAULT;
    return t;
}

static long do_sleep(uint64_t ns, uint64_t rem_ptr)
{
    uint64_t end = sv_now_ns() + ns;
    for (;;) {
        uint64_t now = sv_now_ns();
        if (now >= end) return 0;
        long r = sv_sleep_ms((end - now + 999999) / 1000000);
        if (r < 0) {
            if (rem_ptr) {
                struct ts t;
                uint64_t left = end > sv_now_ns() ? end - sv_now_ns() : 0;
                ns_to_ts(left, &t);
                copy_to_user(rem_ptr, &t, sizeof(t));
            }
            return r;
        }
    }
}

static long s_nanosleep(struct regs *r)
{
    struct ts t;
    if (copy_from_user(&t, A0, sizeof(t))) return -ERR_FAULT;
    if (t.sec < 0 || t.nsec < 0 || t.nsec >= 1000000000) return -ERR_INVAL;
    return do_sleep((uint64_t)t.sec * 1000000000ull + (uint64_t)t.nsec, A1);
}

static long s_clock_nanosleep(struct regs *r)
{
    struct ts t;
    if (copy_from_user(&t, A2, sizeof(t))) return -ERR_FAULT;
    uint64_t want = (uint64_t)t.sec * 1000000000ull + (uint64_t)t.nsec;
    if (A1 & 1) {                                           /* TIMER_ABSTIME */
        struct ts now;
        clock_now((int)A0, &now);
        uint64_t n = (uint64_t)now.sec * 1000000000ull + (uint64_t)now.nsec;
        want = want > n ? want - n : 0;
        return do_sleep(want, 0);
    }
    return do_sleep(want, A3);
}

static long s_sched_yield(struct regs *r) { UNUSED(r); sched_yield(); return 0; }

static long s_times(struct regs *r)
{
    struct sv_group *g = sv_curgrp();
    if (A0) {
        int64_t t[4] = { (int64_t)(cpu_time_ns(g) / 10000000ull), 0, 0, 0 };
        if (copy_to_user(A0, t, sizeof(t))) return -ERR_FAULT;
    }
    return (long)(uptime_ms() / 10);
}

static long s_getrusage(struct regs *r)
{
    uint8_t ru[144];
    memset(ru, 0, sizeof(ru));
    uint64_t ns = (int)A0 == 0 ? cpu_time_ns(sv_curgrp()) : thread_current()->ticks * 1000000ull;
    *(int64_t *)ru = (int64_t)(ns / 1000000000ull);
    *(int64_t *)(ru + 8) = (int64_t)(ns % 1000000000ull / 1000);
    struct mm *m = sv_curgrp()->mm;
    *(int64_t *)(ru + 32) = (int64_t)(m->rss_pages * PAGE_SIZE / 1024);          /* maxrss in KiB */
    return copy_to_user(A1, ru, sizeof(ru)) ? -ERR_FAULT : 0;
}

/* ------------------------------------------------------------------------
 * memory
 * ---------------------------------------------------------------------- */

#define MAP_SHARED 1
#define MAP_PRIVATE 2
#define MAP_FIXED 0x10
#define MAP_ANON 0x20
#define MAP_GROWSDOWN 0x100
#define MAP_NORESERVE 0x4000
#define MAP_POPULATE 0x8000
#define MAP_STACK 0x20000
#define MAP_FIXED_NOREPLACE 0x100000

static long s_mmap(struct regs *r)
{
    uint64_t addr = A0, len = A1, off = A5;
    uint32_t prot = (uint32_t)A2, flags = (uint32_t)A3;
    int fd = (int)A4;
    if (!len || (off & 0xFFF)) return -ERR_INVAL;
    uint32_t vp = (prot & 1 ? VM_READ : 0) | (prot & 2 ? VM_WRITE : 0) | (prot & 4 ? VM_EXEC : 0);
    bool shared = (flags & 0xF) == MAP_SHARED;
    if (!shared && (flags & 0xF) != MAP_PRIVATE) return -ERR_INVAL;
    struct mm *m = sv_curgrp()->mm;
    uint32_t vf = (shared ? VM_SHARED : 0) | ((flags & MAP_GROWSDOWN) ? VM_GROWSDOWN : 0);
    bool fixed = flags & (MAP_FIXED | MAP_FIXED_NOREPLACE);
    if (flags & MAP_ANON) {
        if (shared) {
            /* anonymous shared memory: an object the children of a fork keep sharing */
            struct shm *s = shm_new(ALIGN_UP(len, PAGE_SIZE), "anon");
            if (!s) return -ERR_NOMEM;
            long rc = mm_mmap(m, addr, len, vp, vf, fixed, flags & MAP_FIXED_NOREPLACE, NULL, 0, s, NULL);
            shm_put(s);
            return rc;
        }
        return mm_mmap(m, addr, len, vp, vf, fixed, flags & MAP_FIXED_NOREPLACE, NULL, 0, NULL, NULL);
    }
    struct sfile *f = sv_fd_get(sv_curgrp(), fd);
    if (!f) return -ERR_BADF;
    long rc;
    if (f->ops->mmap) rc = f->ops->mmap(f, m, addr, len, vp, vf | (fixed ? 0x80000000u : 0), fixed, off);
    else if (f->vn && f->vn->type == VN_FILE) {
        char name[32];
        const char *base = vfs_basename(f->path);
        snprintf(name, sizeof(name), "%s", base);
        rc = mm_mmap(m, addr, len, vp, vf, fixed, flags & MAP_FIXED_NOREPLACE, f->vn, off, NULL, name);
    } else rc = -ERR_NODEV;
    sfile_put(f);
    return rc;
}

static long s_munmap(struct regs *r) { return mm_munmap(sv_curgrp()->mm, A0, A1); }

static long s_mprotect(struct regs *r)
{
    uint32_t prot = (uint32_t)A2;
    return mm_mprotect(sv_curgrp()->mm, A0, A1, (prot & 1 ? VM_READ : 0) | (prot & 2 ? VM_WRITE : 0) | (prot & 4 ? VM_EXEC : 0));
}

static long s_brk(struct regs *r) { return mm_brk(sv_curgrp()->mm, A0); }
static long s_mremap(struct regs *r) { return mm_mremap(sv_curgrp()->mm, A0, A1, A2, (int)A3, A4); }
static long s_madvise(struct regs *r) { return mm_madvise(sv_curgrp()->mm, A0, A1, (int)A2); }
static long s_msync(struct regs *r) { return mm_msync(sv_curgrp()->mm, A0, A1); }
static long s_nop0(struct regs *r) { UNUSED(r); return 0; }

static long s_mincore(struct regs *r)
{
    uint64_t pages = (A1 + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint64_t i = 0; i < pages; i += 256) {
        uint8_t v[256];
        uint64_t n = MIN(256ull, pages - i);
        memset(v, 1, n);
        if (copy_to_user(A2 + i, v, n)) return -ERR_FAULT;
    }
    return 0;
}

/* ------------------------------------------------------------------------
 * process identity
 * ---------------------------------------------------------------------- */

static long s_getpid(struct regs *r) { UNUSED(r); return sv_curgrp()->tgid; }
static long s_gettid(struct regs *r) { UNUSED(r); return sv_cur()->tid; }
static long s_getppid(struct regs *r) { UNUSED(r); struct sv_group *g = sv_curgrp(); return g->parent ? g->parent->tgid : g->ppid; }
static long s_getuid(struct regs *r) { UNUSED(r); return sv_curgrp()->uid; }
static long s_geteuid(struct regs *r) { UNUSED(r); return sv_curgrp()->euid; }
static long s_getgid(struct regs *r) { UNUSED(r); return sv_curgrp()->gid; }
static long s_getegid(struct regs *r) { UNUSED(r); return sv_curgrp()->egid; }
static long s_getpgrp(struct regs *r) { UNUSED(r); return sv_curgrp()->pgid; }
static long s_getpgid(struct regs *r) { struct sv_group *g = A0 ? sv_find_group((int)A0) : sv_curgrp(); return g ? g->pgid : -ERR_SRCH; }
static long s_getsid(struct regs *r) { struct sv_group *g = A0 ? sv_find_group((int)A0) : sv_curgrp(); return g ? g->sid : -ERR_SRCH; }

static long s_setpgid(struct regs *r)
{
    struct sv_group *g = A0 ? sv_find_group((int)A0) : sv_curgrp();
    if (!g) return -ERR_SRCH;
    g->pgid = A1 ? (int)A1 : g->tgid;
    return 0;
}

static long s_setsid(struct regs *r)
{
    UNUSED(r);
    struct sv_group *g = sv_curgrp();
    g->sid = g->pgid = g->tgid;
    return g->sid;
}

/* the account the program runs as is the signed-in user's; changing identity is refused unless it changes nothing */
static long s_setuid(struct regs *r) { return (uint32_t)A0 == sv_curgrp()->uid ? 0 : -ERR_PERM; }
static long s_setgid(struct regs *r) { return (uint32_t)A0 == sv_curgrp()->gid ? 0 : -ERR_PERM; }
static long s_ok_if_same(struct regs *r) { UNUSED(r); return 0; }

static long s_getresuid(struct regs *r)
{
    uint32_t v = sv_curgrp()->uid;
    if (copy_to_user(A0, &v, 4) || copy_to_user(A1, &v, 4) || copy_to_user(A2, &v, 4)) return -ERR_FAULT;
    return 0;
}

static long s_getgroups(struct regs *r)
{
    if (!(int)A0) return 1;
    uint32_t g = sv_curgrp()->gid;
    return copy_to_user(A1, &g, 4) ? -ERR_FAULT : 1;
}

static long s_umask(struct regs *r)
{
    struct sv_group *g = sv_curgrp();
    uint32_t old = g->umask;
    g->umask = (uint32_t)A0 & 0777;
    return old;
}

static long s_set_tid_address(struct regs *r)
{
    sv_cur()->clear_child_tid = A0;
    return sv_cur()->tid;
}

static long s_set_robust_list(struct regs *r) { sv_cur()->robust_list = A0; return 0; }
static long s_get_robust_list(struct regs *r) { UNUSED(r); return -ERR_NOSYS; }

static long s_arch_prctl(struct regs *r)
{
    thread_t *th = thread_current();
    switch ((int)A0) {
    case 0x1002:                                            /* ARCH_SET_FS */
        if (!user_range_ok(A1, 0)) return -ERR_INVAL;
        th->fs_base = A1;
        wrmsr(MSR_FS_BASE, A1);
        return 0;
    case 0x1003:                                            /* ARCH_GET_FS */
        return copy_to_user(A1, &th->fs_base, 8) ? -ERR_FAULT : 0;
    case 0x1001: return 0;                                  /* ARCH_SET_GS: not supported, accepted */
    case 0x1004: { uint64_t z = 0; return copy_to_user(A1, &z, 8) ? -ERR_FAULT : 0; }
    default: return -ERR_INVAL;
    }
}

static long s_prctl(struct regs *r)
{
    struct sv_group *g = sv_curgrp();
    switch ((int)A0) {
    case 15: {                                              /* PR_SET_NAME */
        char n[16];
        if (strncpy_from_user(n, A1, 16) < 0) n[15] = 0;
        n[15] = 0;
        strlcpy(g->comm, n, sizeof(g->comm));
        strlcpy(thread_current()->name, n, sizeof(thread_current()->name));
        return 0;
    }
    case 16: return copy_to_user(A1, g->comm, 16) ? -ERR_FAULT : 0;     /* PR_GET_NAME */
    case 1: case 36: case 38: case 4: case 29: case 0x53564d41: return 0;  /* pdeathsig, subreaper, no_new_privs, dumpable, timerslack, vma names */
    case 2: case 3: case 39: { int zero = 0; if ((int)A0 == 2) return copy_to_user(A1, &zero, 4) ? -ERR_FAULT : 0; return (int)A0 == 3 ? 1 : 1; }
    case 23: return 0;                                      /* PR_CAPBSET_READ */
    default: return -ERR_INVAL;
    }
}

/* ------------------------------------------------------------------------
 * limits, system information
 * ---------------------------------------------------------------------- */

static long s_prlimit64(struct regs *r)
{
    struct sv_group *g = A0 ? sv_find_group((int)A0) : sv_curgrp();
    if (!g) return -ERR_SRCH;
    unsigned res = (unsigned)A1;
    if (res >= 16) return -ERR_INVAL;
    uint64_t old[2] = { g->rlim_cur[res], g->rlim_max[res] };
    if (A3 && copy_to_user(A3, old, 16)) return -ERR_FAULT;
    if (A2) {
        uint64_t nl[2];
        if (copy_from_user(nl, A2, 16)) return -ERR_FAULT;
        if (nl[0] > nl[1]) return -ERR_INVAL;
        g->rlim_cur[res] = nl[0];
        g->rlim_max[res] = nl[1];
        if (res == 3) g->mm->rlimit_stack = nl[0] < (1ull << 30) ? nl[0] : (1ull << 30);
        if (res == 9) g->mm->rlimit_as = nl[0];
    }
    return 0;
}

static long s_getrlimit(struct regs *r)
{
    struct sv_group *g = sv_curgrp();
    unsigned res = (unsigned)A0;
    if (res >= 16) return -ERR_INVAL;
    uint64_t v[2] = { g->rlim_cur[res], g->rlim_max[res] };
    return copy_to_user(A1, v, 16) ? -ERR_FAULT : 0;
}

static long s_setrlimit(struct regs *r)
{
    struct sv_group *g = sv_curgrp();
    unsigned res = (unsigned)A0;
    uint64_t v[2];
    if (res >= 16) return -ERR_INVAL;
    if (copy_from_user(v, A1, 16)) return -ERR_FAULT;
    g->rlim_cur[res] = v[0];
    g->rlim_max[res] = v[1];
    if (res == 3) g->mm->rlimit_stack = v[0] < (1ull << 30) ? v[0] : (1ull << 30);
    return 0;
}

static long s_uname(struct regs *r)
{
    char u[6][65];
    memset(u, 0, sizeof(u));
    /* programs written for this interface look at these to decide what they may use */
    strlcpy(u[0], "Linux", 65);
    strlcpy(u[1], sys_hostname(), 65);
    strlcpy(u[2], "6.1.0-zenith", 65);
    strlcpy(u[3], "#1 ZenithOS " ZENITH_VERSION, 65);
    strlcpy(u[4], "x86_64", 65);
    strlcpy(u[5], "(none)", 65);
    return copy_to_user(A0, u, sizeof(u)) ? -ERR_FAULT : 0;
}

static long s_sysinfo(struct regs *r)
{
    uint64_t v[14];
    memset(v, 0, sizeof(v));
    v[0] = uptime_ms() / 1000;
    v[4] = pmm_total_pages() * PAGE_SIZE;
    v[5] = pmm_free_pages() * PAGE_SIZE;
    uint8_t out[112];
    memset(out, 0, sizeof(out));
    memcpy(out, v, 8);                                       /* uptime */
    memcpy(out + 32, &v[4], 8);                              /* totalram */
    memcpy(out + 40, &v[5], 8);                              /* freeram */
    *(uint16_t *)(out + 80) = (uint16_t)sched_thread_count();      /* procs */
    *(uint32_t *)(out + 104) = 1;                            /* mem_unit */
    return copy_to_user(A0, out, sizeof(out)) ? -ERR_FAULT : 0;
}

static long s_getrandom(struct regs *r)
{
    uint64_t n = A1;
    uint64_t p = A0;
    uint64_t done = 0;
    while (done < n) {
        uint8_t buf[256];
        uint64_t c = MIN(sizeof(buf), n - done);
        random_bytes(buf, c);
        if (copy_to_user(p + done, buf, c)) return done ? (long)done : -ERR_FAULT;
        done += c;
    }
    return (long)n;
}

static long s_sched_getaffinity(struct regs *r)
{
    size_t sz = A1;
    if (sz < 8) return -ERR_INVAL;
    uint64_t mask = ncpus >= 64 ? ~0ull : (1ull << ncpus) - 1;
    uint8_t buf[128];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, &mask, 8);
    size_t n = MIN(sz, (size_t)128);
    if (copy_to_user(A2, buf, n)) return -ERR_FAULT;
    return (long)n;
}

static long s_getcpu(struct regs *r)
{
    uint32_t c = (uint32_t)this_cpu()->id, z = 0;
    if (A0 && copy_to_user(A0, &c, 4)) return -ERR_FAULT;
    if (A1 && copy_to_user(A1, &z, 4)) return -ERR_FAULT;
    return 0;
}

static long s_sched_getparam(struct regs *r) { uint32_t z = 0; return A1 && copy_to_user(A1, &z, 4) ? -ERR_FAULT : 0; }
static long s_sched_get_priority(struct regs *r) { return (int)A0 == 1 || (int)A0 == 2 ? 99 : 0; }

/* ------------------------------------------------------------------------
 * processes
 * ---------------------------------------------------------------------- */

static long s_exit(struct regs *r) { sv_exit_task((int)((A0 & 0xFF) << 8), false); }
static long s_exit_group(struct regs *r) { sv_exit_task((int)((A0 & 0xFF) << 8), true); }

static long s_clone(struct regs *r) { return sv_clone(r, A0, A1, A2, A3, A4); }
static long s_fork(struct regs *r) { return sv_clone(r, 17, 0, 0, 0, 0); }                 /* SIGCHLD */
static long s_vfork(struct regs *r) { return sv_clone(r, 0x100 | 0x4000 | 17, 0, 0, 0, 0); }   /* CLONE_VM | CLONE_VFORK | SIGCHLD */

struct cl_args {
    uint64_t flags, pidfd, child_tid, parent_tid, exit_signal, stack, stack_size, tls, set_tid, set_tid_size, cgroup;
};

static long s_clone3(struct regs *r)
{
    struct cl_args a;
    memset(&a, 0, sizeof(a));
    if (A1 < 64 || A1 > sizeof(a)) return -ERR_INVAL;
    if (copy_from_user(&a, A0, A1)) return -ERR_FAULT;
    uint64_t sp = a.stack ? a.stack + a.stack_size : 0;
    return sv_clone(r, (a.flags & ~0xFFull) | (a.exit_signal & 0xFF), sp, a.parent_tid, a.child_tid, a.tls);
}

static long s_execve(struct regs *r)
{
    char path[VFS_PATH_MAX];
    long n = strncpy_from_user(path, A0, sizeof(path));
    if (n < 0) return n;
    char **argv, **envp;
    long argc = copy_string_array(A1, &argv);
    if (argc < 0) return argc;
    long envc = copy_string_array(A2, &envp);
    if (envc < 0) { free_string_array(argv, argc); return envc; }
    char abs[VFS_PATH_MAX];
    vfs_resolve(sv_curgrp()->cwd, path, abs, sizeof(abs));
    char err[160] = "";
    int rc = sv_exec_file(abs, argv, envp, r, false, err, sizeof(err));
    free_string_array(argv, argc);
    free_string_array(envp, envc);
    if (rc && err[0]) klog("abi: execve %s: %s", abs, err);
    return rc;
}

static long s_execveat(struct regs *r)
{
    /* only the absolute-path and cwd forms: execveat(fd, "", ..., AT_EMPTY_PATH) is rarely used */
    struct regs tmp = *r;
    tmp.rdi = A1; tmp.rsi = A2; tmp.rdx = A3;
    long rc = s_execve(&tmp);
    if (rc == 0) *r = tmp;
    return rc;
}

static long s_wait4(struct regs *r)
{
    int st = 0;
    long rc = sv_wait((int)A0, &st, (int)A2, A3);
    if (rc > 0 && A1 && copy_to_user(A1, &st, 4)) return -ERR_FAULT;
    return rc;
}

static long s_waitid(struct regs *r)
{
    int idtype = (int)A0, id = (int)A1, opts = (int)A3;
    int pid = idtype == 0 ? -1 : idtype == 1 ? id : -id;                 /* P_ALL, P_PID, P_PGID */
    int st = 0;
    long rc = sv_wait(pid, &st, (opts & 1 ? 1 : 0) | (opts & 4 ? 0 : 0), 0);
    if (rc < 0) return rc;
    if (A2) {
        uint8_t si[128];
        memset(si, 0, sizeof(si));
        if (rc > 0) {
            *(int32_t *)si = 17;                                         /* SIGCHLD */
            *(int32_t *)(si + 8) = (st & 0x7F) ? 2 : 1;                  /* CLD_KILLED / CLD_EXITED */
            *(int32_t *)(si + 16) = (int32_t)rc;
            *(int32_t *)(si + 20) = (int32_t)sv_curgrp()->uid;
            *(int32_t *)(si + 24) = (st & 0x7F) ? (st & 0x7F) : (st >> 8);
        }
        if (copy_to_user(A2, si, sizeof(si))) return -ERR_FAULT;
    }
    return 0;
}

/* ------------------------------------------------------------------------
 * signals
 * ---------------------------------------------------------------------- */

static long s_kill(struct regs *r)
{
    int pid = (int)A0, sig = (int)A1;
    if (sig < 0 || sig > NSIG) return -ERR_INVAL;
    if (pid > 0) return sv_kill_group(pid, sig);
    if (pid == 0 || pid < -1) {                                          /* a process group */
        int pg = pid == 0 ? sv_curgrp()->pgid : -pid;
        int ids[128];
        int n = sv_group_list(ids, 128), hit = 0;
        for (int i = 0; i < n; i++) {
            struct sv_group *g = sv_find_group(ids[i]);
            if (g && g->pgid == pg) { if (sig) sv_send_signal(g, NULL, sig); hit++; }
        }
        return hit ? 0 : -ERR_SRCH;
    }
    return -ERR_PERM;
}

static long s_tkill(struct regs *r) { return sv_kill_task(0, (int)A0, (int)A1); }
static long s_tgkill(struct regs *r)
{
    struct sv_task *t = sv_find_task((int)A1);
    if (t && t == sv_cur() && A2) t->tgkill_sig = (int)A2;
    return sv_kill_task((int)A0, (int)A1, (int)A2);
}

static long s_rt_sigaction(struct regs *r) { return sv_sigaction((int)A0, A1, A2, A3); }
static long s_rt_sigprocmask(struct regs *r) { return sv_sigprocmask((int)A0, A1, A2, A3); }
static long s_rt_sigreturn(struct regs *r) { return sv_rt_sigreturn(r); }
static long s_sigaltstack(struct regs *r) { return sv_sigaltstack(A0, A1); }
static long s_rt_sigsuspend(struct regs *r) { return sv_sigsuspend(A0, A1); }
static long s_rt_sigpending(struct regs *r) { return sv_sigpending(A0, A1); }

static long s_pause(struct regs *r)
{
    UNUSED(r);
    struct sv_task *t = sv_cur();
    for (;;) {
        spin_lock(&sv_lock);
        int w = sv_wait_chan(t, &sv_lock, 0);
        spin_unlock(&sv_lock);
        if (w < 0) return w;
    }
}

static long s_rt_sigtimedwait(struct regs *r)
{
    struct sv_task *t = sv_cur();
    sigset64_t set;
    if (copy_from_user(&set, A0, 8)) return -ERR_FAULT;
    uint64_t timeout_ms = 0;
    bool have_timeout = false;
    if (A2) {
        struct ts ts;
        if (copy_from_user(&ts, A2, sizeof(ts))) return -ERR_FAULT;
        timeout_ms = (uint64_t)ts.sec * 1000 + (uint64_t)ts.nsec / 1000000;
        have_timeout = true;
    }
    uint64_t start = uptime_ms();
    for (;;) {
        spin_lock(&sv_lock);
        sigset64_t avail = (t->pending | t->g->pending) & set;
        if (avail) {
            int sig = __builtin_ctzll(avail) + 1;
            if (t->pending & SIGBIT(sig)) t->pending &= ~SIGBIT(sig); else t->g->pending &= ~SIGBIT(sig);
            spin_unlock(&sv_lock);
            if (A1) {
                uint8_t si[128];
                memset(si, 0, sizeof(si));
                *(int32_t *)si = sig;
                copy_to_user(A1, si, sizeof(si));
            }
            return sig;
        }
        uint64_t left = 0;
        if (have_timeout) {
            uint64_t el = uptime_ms() - start;
            if (el >= timeout_ms) { spin_unlock(&sv_lock); return -ERR_AGAIN; }
            left = timeout_ms - el;
        }
        t->blocked |= 0;      /* the waited-for signals are blocked by the caller */
        int w = sv_wait_chan(t, &sv_lock, left);
        spin_unlock(&sv_lock);
        if (w < 0 && !((t->pending | t->g->pending) & set)) return w;
    }
}

/* ------------------------------------------------------------------------
 * the table
 * ---------------------------------------------------------------------- */

typedef long (*sysfn)(struct regs *r);

#define NSYS 450
static sysfn table[NSYS];
static bool table_ready;

enum {
    N_read = 0, N_write, N_open, N_close, N_stat, N_fstat, N_lstat, N_poll, N_lseek, N_mmap, N_mprotect, N_munmap, N_brk,
    N_rt_sigaction, N_rt_sigprocmask, N_rt_sigreturn, N_ioctl, N_pread64, N_pwrite64, N_readv, N_writev, N_access, N_pipe,
    N_select, N_sched_yield, N_mremap, N_msync, N_mincore, N_madvise, N_shmget, N_shmat, N_shmctl, N_dup, N_dup2, N_pause,
    N_nanosleep, N_getitimer, N_alarm, N_setitimer, N_getpid, N_sendfile, N_socket, N_connect, N_accept, N_sendto,
    N_recvfrom, N_sendmsg, N_recvmsg, N_shutdown, N_bind, N_listen, N_getsockname, N_getpeername, N_socketpair,
    N_setsockopt, N_getsockopt, N_clone, N_fork, N_vfork, N_execve, N_exit, N_wait4, N_kill, N_uname,
    N_fcntl = 72, N_flock, N_fsync, N_fdatasync, N_truncate, N_ftruncate, N_getdents, N_getcwd, N_chdir, N_fchdir,
    N_rename, N_mkdir, N_rmdir, N_creat, N_link, N_unlink, N_symlink, N_readlink, N_chmod, N_fchmod, N_chown, N_fchown,
    N_lchown, N_umask, N_gettimeofday, N_getrlimit, N_getrusage, N_sysinfo, N_times, N_ptrace, N_getuid, N_syslog,
    N_getgid, N_setuid, N_setgid, N_geteuid, N_getegid, N_setpgid, N_getppid, N_getpgrp, N_setsid, N_setreuid,
    N_setregid, N_getgroups, N_setgroups, N_setresuid, N_getresuid, N_setresgid, N_getresgid, N_getpgid, N_setfsuid,
    N_setfsgid, N_getsid, N_capget, N_capset, N_rt_sigpending, N_rt_sigtimedwait, N_rt_sigqueueinfo, N_rt_sigsuspend,
    N_sigaltstack, N_utime, N_mknod, N_uselib, N_personality, N_ustat, N_statfs, N_fstatfs,
    N_getpriority = 140, N_setpriority, N_sched_setparam, N_sched_getparam, N_sched_setscheduler, N_sched_getscheduler,
    N_sched_get_priority_max, N_sched_get_priority_min, N_sched_rr_get_interval, N_mlock, N_munlock, N_mlockall,
    N_munlockall,
    N_prctl = 157, N_arch_prctl, N_adjtimex, N_setrlimit, N_chroot, N_sync, N_acct, N_settimeofday,
    N_gettid = 186, N_readahead, N_setxattr, N_lsetxattr, N_fsetxattr, N_getxattr, N_lgetxattr, N_fgetxattr,
    N_listxattr, N_llistxattr, N_flistxattr, N_removexattr, N_lremovexattr, N_fremovexattr, N_tkill, N_time, N_futex,
    N_sched_setaffinity, N_sched_getaffinity,
    N_epoll_create = 213, N_getdents64 = 217, N_set_tid_address, N_restart_syscall, N_semtimedop, N_fadvise64,
    N_timer_create, N_timer_settime, N_timer_gettime, N_timer_getoverrun, N_timer_delete, N_clock_settime,
    N_clock_gettime, N_clock_getres, N_clock_nanosleep, N_exit_group, N_epoll_wait, N_epoll_ctl, N_tgkill, N_utimes,
    N_waitid = 247, N_inotify_init = 253, N_inotify_add_watch, N_inotify_rm_watch,
    N_openat = 257, N_mkdirat, N_mknodat, N_fchownat, N_futimesat, N_newfstatat, N_unlinkat, N_renameat, N_linkat,
    N_symlinkat, N_readlinkat, N_fchmodat, N_faccessat, N_pselect6, N_ppoll, N_unshare, N_set_robust_list,
    N_get_robust_list, N_splice, N_tee, N_sync_file_range, N_vmsplice, N_move_pages, N_utimensat, N_epoll_pwait,
    N_signalfd, N_timerfd_create, N_eventfd, N_fallocate, N_timerfd_settime, N_timerfd_gettime, N_accept4,
    N_signalfd4, N_eventfd2, N_epoll_create1, N_dup3, N_pipe2, N_inotify_init1, N_preadv, N_pwritev,
    N_rt_tgsigqueueinfo, N_perf_event_open, N_recvmmsg, N_fanotify_init, N_fanotify_mark, N_prlimit64,
    N_name_to_handle_at, N_open_by_handle_at, N_clock_adjtime, N_syncfs, N_sendmmsg, N_setns, N_getcpu,
    N_process_vm_readv, N_process_vm_writev, N_kcmp, N_finit_module, N_sched_setattr, N_sched_getattr, N_renameat2,
    N_seccomp, N_getrandom, N_memfd_create, N_kexec_file_load, N_bpf, N_execveat, N_userfaultfd, N_membarrier,
    N_mlock2, N_copy_file_range, N_preadv2, N_pwritev2, N_pkey_mprotect, N_pkey_alloc, N_pkey_free, N_statx,
    N_io_pgetevents, N_rseq,
    N_pidfd_send_signal = 424, N_io_uring_setup, N_io_uring_enter, N_io_uring_register, N_open_tree, N_move_mount,
    N_fsopen, N_fsconfig, N_fsmount, N_fspick, N_pidfd_open, N_clone3, N_close_range, N_openat2, N_pidfd_getfd,
    N_faccessat2, N_process_madvise, N_epoll_pwait2,
};

static long s_enosys(struct regs *r) { UNUSED(r); return -ERR_NOSYS; }
static long s_eperm(struct regs *r) { UNUSED(r); return -ERR_PERM; }
static long s_einval(struct regs *r) { UNUSED(r); return -ERR_INVAL; }

static void build_table(void)
{
    for (int i = 0; i < NSYS; i++) table[i] = s_enosys;
#define T(n, f) table[n] = f
    /* files and descriptors (fsys.c) */
    T(N_read, s_read); T(N_write, s_write); T(N_open, s_open); T(N_close, s_close); T(N_stat, s_stat); T(N_fstat, s_fstat);
    T(N_lstat, s_lstat); T(N_lseek, s_lseek); T(N_ioctl, s_ioctl); T(N_pread64, s_pread64); T(N_pwrite64, s_pwrite64);
    T(N_readv, s_readv); T(N_writev, s_writev); T(N_access, s_access); T(N_dup, s_dup); T(N_dup2, s_dup2);
    T(N_fcntl, s_fcntl); T(N_flock, s_nop0); T(N_fsync, s_nop0); T(N_fdatasync, s_nop0); T(N_truncate, s_truncate);
    T(N_ftruncate, s_ftruncate); T(N_getdents, s_getdents); T(N_getdents64, s_getdents64); T(N_getcwd, s_getcwd);
    T(N_chdir, s_chdir); T(N_fchdir, s_fchdir); T(N_rename, s_rename); T(N_mkdir, s_mkdir); T(N_rmdir, s_rmdir);
    T(N_creat, s_creat); T(N_unlink, s_unlink); T(N_readlink, s_readlink); T(N_chmod, s_nop0); T(N_fchmod, s_nop0);
    T(N_chown, s_nop0); T(N_fchown, s_nop0); T(N_lchown, s_nop0); T(N_openat, s_openat); T(N_mkdirat, s_mkdirat);
    T(N_newfstatat, s_newfstatat); T(N_unlinkat, s_unlinkat); T(N_renameat, s_renameat); T(N_renameat2, s_renameat2);
    T(N_readlinkat, s_readlinkat); T(N_fchmodat, s_nop0); T(N_faccessat, s_faccessat); T(N_faccessat2, s_faccessat);
    T(N_utimensat, s_nop0); T(N_utimes, s_nop0); T(N_utime, s_nop0); T(N_futimesat, s_nop0);
    T(N_dup3, s_dup3); T(N_pipe, s_pipe); T(N_pipe2, s_pipe2); T(N_statx, s_statx); T(N_statfs, s_statfs);
    T(N_fstatfs, s_fstatfs); T(N_fadvise64, s_nop0); T(N_fallocate, s_fallocate); T(N_sendfile, s_sendfile);
    T(N_close_range, s_close_range); T(N_copy_file_range, s_copy_file_range); T(N_symlink, s_enosys);
    T(N_link, s_enosys); T(N_symlinkat, s_enosys); T(N_linkat, s_enosys); T(N_sync, s_nop0); T(N_syncfs, s_nop0);
    T(N_preadv, s_preadv); T(N_pwritev, s_pwritev); T(N_preadv2, s_preadv); T(N_pwritev2, s_pwritev);
    T(N_memfd_create, s_memfd_create); T(N_readahead, s_nop0); T(N_sync_file_range, s_nop0);
    T(N_getxattr, s_enodata); T(N_lgetxattr, s_enodata); T(N_fgetxattr, s_enodata); T(N_listxattr, s_nop0);
    T(N_llistxattr, s_nop0); T(N_flistxattr, s_nop0); T(N_setxattr, s_enosys);
    /* polling and events (poll.c) */
    T(N_poll, s_poll); T(N_ppoll, s_ppoll); T(N_select, s_select); T(N_pselect6, s_pselect6);
    T(N_epoll_create, s_epoll_create); T(N_epoll_create1, s_epoll_create1); T(N_epoll_ctl, s_epoll_ctl);
    T(N_epoll_wait, s_epoll_wait); T(N_epoll_pwait, s_epoll_pwait); T(N_epoll_pwait2, s_epoll_pwait2);
    T(N_eventfd, s_eventfd); T(N_eventfd2, s_eventfd2); T(N_timerfd_create, s_timerfd_create);
    T(N_timerfd_settime, s_timerfd_settime); T(N_timerfd_gettime, s_timerfd_gettime); T(N_signalfd, s_signalfd);
    T(N_signalfd4, s_signalfd4); T(N_inotify_init, s_inotify_init); T(N_inotify_init1, s_inotify_init);
    T(N_inotify_add_watch, s_inotify_add_watch); T(N_inotify_rm_watch, s_nop0);
    /* sockets (sock.c) */
    T(N_socket, s_socket); T(N_connect, s_connect); T(N_accept, s_accept); T(N_accept4, s_accept4);
    T(N_sendto, s_sendto); T(N_recvfrom, s_recvfrom); T(N_sendmsg, s_sendmsg); T(N_recvmsg, s_recvmsg);
    T(N_shutdown, s_shutdown); T(N_bind, s_bind); T(N_listen, s_listen); T(N_getsockname, s_getsockname);
    T(N_getpeername, s_getpeername); T(N_socketpair, s_socketpair); T(N_setsockopt, s_setsockopt);
    T(N_getsockopt, s_getsockopt); T(N_sendmmsg, s_sendmmsg); T(N_recvmmsg, s_recvmmsg);
    /* memory */
    T(N_mmap, s_mmap); T(N_mprotect, s_mprotect); T(N_munmap, s_munmap); T(N_brk, s_brk); T(N_mremap, s_mremap);
    T(N_msync, s_msync); T(N_mincore, s_mincore); T(N_madvise, s_madvise); T(N_mlock, s_nop0); T(N_munlock, s_nop0);
    T(N_mlockall, s_nop0); T(N_munlockall, s_nop0); T(N_mlock2, s_nop0); T(N_membarrier, s_nop0);
    T(N_shmget, s_shmget); T(N_shmat, s_shmat); T(N_shmctl, s_shmctl);
    /* processes and threads */
    T(N_getpid, s_getpid); T(N_gettid, s_gettid); T(N_getppid, s_getppid); T(N_getuid, s_getuid); T(N_geteuid, s_geteuid);
    T(N_getgid, s_getgid); T(N_getegid, s_getegid); T(N_getpgrp, s_getpgrp); T(N_getpgid, s_getpgid); T(N_getsid, s_getsid);
    T(N_setpgid, s_setpgid); T(N_setsid, s_setsid); T(N_setuid, s_setuid); T(N_setgid, s_setgid);
    T(N_setreuid, s_ok_if_same); T(N_setregid, s_ok_if_same); T(N_setresuid, s_ok_if_same); T(N_setresgid, s_ok_if_same);
    T(N_setfsuid, s_getuid); T(N_setfsgid, s_getgid); T(N_getresuid, s_getresuid); T(N_getresgid, s_getresuid);
    T(N_getgroups, s_getgroups); T(N_setgroups, s_ok_if_same); T(N_umask, s_umask);
    T(N_set_tid_address, s_set_tid_address); T(N_set_robust_list, s_set_robust_list); T(N_get_robust_list, s_get_robust_list);
    T(N_arch_prctl, s_arch_prctl); T(N_prctl, s_prctl); T(N_prlimit64, s_prlimit64); T(N_getrlimit, s_getrlimit);
    T(N_setrlimit, s_setrlimit); T(N_uname, s_uname); T(N_sysinfo, s_sysinfo); T(N_getrandom, s_getrandom);
    T(N_sched_getaffinity, s_sched_getaffinity); T(N_sched_setaffinity, s_nop0); T(N_getcpu, s_getcpu);
    T(N_sched_yield, s_sched_yield); T(N_sched_getparam, s_sched_getparam); T(N_sched_setparam, s_nop0);
    T(N_sched_setscheduler, s_nop0); T(N_sched_getscheduler, s_nop0); T(N_sched_get_priority_max, s_sched_get_priority);
    T(N_sched_get_priority_min, s_nop0); T(N_getpriority, s_nop0); T(N_setpriority, s_nop0);
    T(N_clone, s_clone); T(N_clone3, s_clone3); T(N_fork, s_fork); T(N_vfork, s_vfork); T(N_execve, s_execve);
    T(N_execveat, s_execveat); T(N_exit, s_exit); T(N_exit_group, s_exit_group); T(N_wait4, s_wait4);
    T(N_waitid, s_waitid); T(N_futex, s_futex); T(N_rseq, s_enosys); T(N_capget, s_capget); T(N_capset, s_nop0);
    T(N_personality, s_nop0); T(N_ptrace, s_eperm); T(N_seccomp, s_einval); T(N_getrusage, s_getrusage);
    T(N_times, s_times); T(N_syslog, s_nop0); T(N_chroot, s_eperm); T(N_unshare, s_eperm); T(N_setns, s_eperm);
    T(N_perf_event_open, s_eperm); T(N_bpf, s_eperm); T(N_userfaultfd, s_eperm); T(N_io_uring_setup, s_enosys);
    T(N_kcmp, s_enosys);
    /* signals */
    T(N_kill, s_kill); T(N_tkill, s_tkill); T(N_tgkill, s_tgkill); T(N_rt_sigaction, s_rt_sigaction);
    T(N_rt_sigprocmask, s_rt_sigprocmask); T(N_rt_sigreturn, s_rt_sigreturn); T(N_sigaltstack, s_sigaltstack);
    T(N_rt_sigsuspend, s_rt_sigsuspend); T(N_rt_sigpending, s_rt_sigpending); T(N_pause, s_pause);
    T(N_rt_sigtimedwait, s_rt_sigtimedwait); T(N_alarm, s_alarm); T(N_setitimer, s_setitimer); T(N_getitimer, s_getitimer);
    T(N_timer_create, s_timer_create); T(N_timer_settime, s_timer_settime); T(N_timer_gettime, s_timer_gettime);
    T(N_timer_delete, s_nop0); T(N_timer_getoverrun, s_nop0);
    T(N_pidfd_send_signal, s_enosys); T(N_pidfd_open, s_enosys);
    /* time */
    T(N_clock_gettime, s_clock_gettime); T(N_clock_getres, s_clock_getres); T(N_clock_settime, s_clock_settime);
    T(N_gettimeofday, s_gettimeofday); T(N_time, s_time); T(N_nanosleep, s_nanosleep); T(N_clock_nanosleep, s_clock_nanosleep);
    T(N_settimeofday, s_eperm); T(N_adjtimex, s_eperm); T(N_clock_adjtime, s_eperm);
#undef T
    table_ready = true;
}

bool sv_abi_ready(void) { return true; }

static int trace_mode = -1;

void sv_syscall(struct regs *r)
{
    if (!table_ready) build_table();
    if (trace_mode < 0) trace_mode = (int)conf_int(&sysconf, "abi_trace", 0);        /* a debugging aid: log every call */
    struct sv_task *t = sv_cur();
    uint64_t nr = r->rax;
    thread_current()->interrupted = 0;
    t->syscall_nr = (int)nr;
    long rc;
    uint64_t a[6] = { r->rdi, r->rsi, r->rdx, r->r10, r->r8, r->r9 };
    bool tr = trace_mode == 1 || (trace_mode == 2 && (nr == N_clone || nr == N_futex || nr == N_exit || nr == N_exit_group || nr == N_wait4 ||
                                                     nr == N_kill || nr == N_tgkill || nr == N_rt_sigreturn || nr == N_fork || nr == N_clone3));
    if (tr) klog("sv %d.%d: call %lu(%#lx, %#lx, %#lx, %#lx)", t->g->tgid, t->tid, nr, a[0], a[1], a[2], a[3]);
    if (nr < NSYS) rc = table[nr](r);
    else rc = -ERR_NOSYS;
    if (tr && nr != N_rt_sigreturn) klog("sv %d.%d: call %lu = %ld", t->g->tgid, t->tid, nr, rc);
    if (nr == N_rt_sigreturn) { t->syscall_nr = -1; r->rax = (uint64_t)rc; return; }
    r->rax = (uint64_t)rc;
    if (rc != -ERR_INTR) t->syscall_nr = -1;
}

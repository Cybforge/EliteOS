/*
 * Timers, capabilities, System V shared memory.
 */
#include <kernel.h>
#include <sched.h>
#include <mm.h>
#include <dev.h>
#include <umm.h>
#include <abierr.h>
#include "sv.h"
#include "svcalls.h"

extern spinlock_t sv_lock;

/* ------------------------------------------------------------------------
 * interval timers (real time) and POSIX timers: one thread watches the clock
 * ---------------------------------------------------------------------- */

#define MAX_PTIMERS 64

struct ptimer {
    int id;
    struct sv_group *g;
    int sig;
    uint64_t next_ns, interval_ns;
    bool armed, used;
};

static struct ptimer ptimers[MAX_PTIMERS];
static bool timer_thread_started;

static int timer_main(void *arg)
{
    UNUSED(arg);
    for (;;) {
        sched_sleep(5);
        uint64_t now = sv_now_ns();
        int ids[64];
        int n = sv_group_list(ids, 64);
        for (int i = 0; i < n; i++) {
            struct sv_group *g = sv_find_group(ids[i]);
            if (!g || !g->alarm_at || now < g->alarm_at) continue;
            sv_send_signal(g, NULL, SIGALRM);
            g->alarm_at = g->itimer_interval ? now + g->itimer_interval : 0;
        }
        for (int i = 0; i < MAX_PTIMERS; i++) {
            struct ptimer *t = &ptimers[i];
            if (!t->armed || now < t->next_ns) continue;
            if (t->g && !t->g->zombie && t->sig) sv_send_signal(t->g, NULL, t->sig);
            if (t->interval_ns) t->next_ns = now + t->interval_ns;
            else t->armed = false;
        }
        sv_timerfd_tick(now);
    }
    return 0;
}

void sv_ensure_timer_thread(void)
{
    if (timer_thread_started) return;
    timer_thread_started = true;
    thread_create("sv-timers", timer_main, NULL);
}

struct ts2 { int64_t sec, nsec; };

static uint64_t ts_ns(const struct ts2 *t) { return (uint64_t)t->sec * 1000000000ull + (uint64_t)t->nsec; }

SC(s_alarm)
{
    struct sv_group *g = sv_curgrp();
    sv_ensure_timer_thread();
    uint64_t now = sv_now_ns();
    long left = g->alarm_at > now ? (long)((g->alarm_at - now + 999999999) / 1000000000) : 0;
    g->alarm_at = r->rdi ? now + r->rdi * 1000000000ull : 0;
    g->itimer_interval = 0;
    return left;
}

SC(s_setitimer)
{
    struct sv_group *g = sv_curgrp();
    if ((int)r->rdi != 0) return 0;                          /* only ITIMER_REAL is delivered */
    sv_ensure_timer_thread();
    uint64_t now = sv_now_ns();
    if (r->rdx) {
        struct { struct { int64_t s, us; } interval, value; } old;
        old.value.s = g->alarm_at > now ? (int64_t)((g->alarm_at - now) / 1000000000ull) : 0;
        old.value.us = g->alarm_at > now ? (int64_t)((g->alarm_at - now) % 1000000000ull / 1000) : 0;
        old.interval.s = (int64_t)(g->itimer_interval / 1000000000ull);
        old.interval.us = (int64_t)(g->itimer_interval % 1000000000ull / 1000);
        if (copy_to_user(r->rdx, &old, sizeof(old))) return -ERR_FAULT;
    }
    if (r->rsi) {
        struct { struct { int64_t s, us; } interval, value; } nv;
        if (copy_from_user(&nv, r->rsi, sizeof(nv))) return -ERR_FAULT;
        uint64_t v = (uint64_t)nv.value.s * 1000000000ull + (uint64_t)nv.value.us * 1000;
        g->itimer_interval = (uint64_t)nv.interval.s * 1000000000ull + (uint64_t)nv.interval.us * 1000;
        g->alarm_at = v ? now + v : 0;
    }
    return 0;
}

SC(s_getitimer)
{
    struct sv_group *g = sv_curgrp();
    uint64_t now = sv_now_ns();
    struct { struct { int64_t s, us; } interval, value; } v;
    memset(&v, 0, sizeof(v));
    if ((int)r->rdi == 0) {
        v.value.s = g->alarm_at > now ? (int64_t)((g->alarm_at - now) / 1000000000ull) : 0;
        v.value.us = g->alarm_at > now ? (int64_t)((g->alarm_at - now) % 1000000000ull / 1000) : 0;
        v.interval.s = (int64_t)(g->itimer_interval / 1000000000ull);
        v.interval.us = (int64_t)(g->itimer_interval % 1000000000ull / 1000);
    }
    return copy_to_user(r->rsi, &v, sizeof(v)) ? -ERR_FAULT : 0;
}

SC(s_timer_create)
{
    struct sv_group *g = sv_curgrp();
    sv_ensure_timer_thread();
    int sig = SIGALRM;
    if (r->rsi) {
        struct { uint64_t value; int32_t signo, notify; } ev;          /* struct sigevent (the part that matters) */
        if (copy_from_user(&ev, r->rsi, sizeof(ev))) return -ERR_FAULT;
        if (ev.notify == 1) sig = 0;                                   /* SIGEV_NONE */
        else if (ev.notify == 0) sig = ev.signo;                       /* SIGEV_SIGNAL */
        else return -ERR_INVAL;                                        /* thread notification is not supported */
    }
    for (int i = 0; i < MAX_PTIMERS; i++)
        if (!ptimers[i].used) {
            ptimers[i] = (struct ptimer){ .id = i, .g = g, .sig = sig, .used = true };
            return copy_to_user(r->rdx, &i, 4) ? -ERR_FAULT : 0;
        }
    return -ERR_AGAIN;
}

SC(s_timer_settime)
{
    int id = (int)r->rdi;
    if (id < 0 || id >= MAX_PTIMERS || !ptimers[id].used) return -ERR_INVAL;
    struct { struct ts2 interval, value; } nv;
    if (copy_from_user(&nv, r->rdx, sizeof(nv))) return -ERR_FAULT;
    struct ptimer *t = &ptimers[id];
    uint64_t now = sv_now_ns();
    uint64_t v = ts_ns(&nv.value);
    if (r->rsi & 1) v = v > now ? v - now : 0;                          /* TIMER_ABSTIME (monotonic) */
    t->interval_ns = ts_ns(&nv.interval);
    t->next_ns = now + v;
    t->armed = nv.value.sec || nv.value.nsec;
    return 0;
}

SC(s_timer_gettime)
{
    int id = (int)r->rdi;
    if (id < 0 || id >= MAX_PTIMERS || !ptimers[id].used) return -ERR_INVAL;
    struct { struct ts2 interval, value; } v;
    memset(&v, 0, sizeof(v));
    uint64_t now = sv_now_ns();
    struct ptimer *t = &ptimers[id];
    if (t->armed && t->next_ns > now) { uint64_t d = t->next_ns - now; v.value.sec = (int64_t)(d / 1000000000ull); v.value.nsec = (int64_t)(d % 1000000000ull); }
    v.interval.sec = (int64_t)(t->interval_ns / 1000000000ull);
    v.interval.nsec = (int64_t)(t->interval_ns % 1000000000ull);
    return copy_to_user(r->rsi, &v, sizeof(v)) ? -ERR_FAULT : 0;
}

/* ------------------------------------------------------------------------
 * capabilities: the program is an ordinary user's, it has none
 * ---------------------------------------------------------------------- */

SC(s_capget)
{
    struct { uint32_t version; int32_t pid; } hdr;
    if (copy_from_user(&hdr, r->rdi, sizeof(hdr))) return -ERR_FAULT;
    if (hdr.version != 0x20080522 && hdr.version != 0x20071026 && hdr.version != 0x19980330) {
        hdr.version = 0x20080522;
        copy_to_user(r->rdi, &hdr, sizeof(hdr));
        return -ERR_INVAL;
    }
    if (r->rsi) {
        uint32_t d[6];
        memset(d, 0, sizeof(d));
        if (copy_to_user(r->rsi, d, hdr.version == 0x19980330 ? 12 : 24)) return -ERR_FAULT;
    }
    return 0;
}

/* ------------------------------------------------------------------------
 * System V shared memory
 * ---------------------------------------------------------------------- */

#define MAX_SHM 64

struct shmseg {
    bool used;
    int32_t key;
    struct shm *shm;
    int nattach;
    bool removed;
};

static struct shmseg shmsegs[MAX_SHM];
static mutex_t shm_lock = MUTEX_INIT("sysvshm");

SC(s_shmget)
{
    int32_t key = (int32_t)r->rdi;
    uint64_t size = r->rsi;
    int flg = (int)r->rdx;
    mutex_lock(&shm_lock);
    if (key != 0) {
        for (int i = 0; i < MAX_SHM; i++)
            if (shmsegs[i].used && shmsegs[i].key == key) {
                mutex_unlock(&shm_lock);
                if ((flg & 01000) && (flg & 02000)) return -ERR_EXIST;
                return i;
            }
        if (!(flg & 01000)) { mutex_unlock(&shm_lock); return -ERR_NOENT; }
    }
    for (int i = 0; i < MAX_SHM; i++)
        if (!shmsegs[i].used) {
            struct shm *s = shm_new(ALIGN_UP(size, PAGE_SIZE), "sysv");
            if (!s) { mutex_unlock(&shm_lock); return -ERR_NOMEM; }
            shmsegs[i] = (struct shmseg){ .used = true, .key = key, .shm = s };
            mutex_unlock(&shm_lock);
            return i;
        }
    mutex_unlock(&shm_lock);
    return -ERR_NOSPC;
}

SC(s_shmat)
{
    int id = (int)r->rdi;
    if (id < 0 || id >= MAX_SHM || !shmsegs[id].used) return -ERR_INVAL;
    struct shm *s = shmsegs[id].shm;
    uint32_t prot = VM_READ | ((r->rdx & 010000) ? 0 : VM_WRITE);        /* SHM_RDONLY */
    long a = mm_mmap(sv_curgrp()->mm, r->rsi, s->npages * PAGE_SIZE, prot, VM_SHARED, r->rsi != 0, false, NULL, 0, s, "/SYSV");
    if (a > 0) shmsegs[id].nattach++;
    return a;
}

SC(s_shmctl)
{
    int id = (int)r->rdi;
    if (id < 0 || id >= MAX_SHM || !shmsegs[id].used) return -ERR_INVAL;
    int cmd = (int)r->rsi;
    if (cmd == 0) {                                                        /* IPC_RMID */
        shm_put(shmsegs[id].shm);
        shmsegs[id].used = false;
        return 0;
    }
    if (cmd == 2) {                                                        /* IPC_STAT */
        uint8_t ds[112];
        memset(ds, 0, sizeof(ds));
        *(uint64_t *)(ds + 48) = shmsegs[id].shm->size;
        *(uint64_t *)(ds + 88) = (uint64_t)shmsegs[id].nattach;
        return r->rdx && copy_to_user(r->rdx, ds, sizeof(ds)) ? -ERR_FAULT : 0;
    }
    return 0;
}

/*
 * Futexes: wait and wake keyed by the physical address of a word of user
 * memory, so private and shared mappings behave the same way.
 */
#include <kernel.h>
#include <sched.h>
#include <mm.h>
#include <dev.h>
#include <umm.h>
#include <abierr.h>
#include "sv.h"
#include "svcalls.h"

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_REQUEUE 3
#define FUTEX_CMP_REQUEUE 4
#define FUTEX_WAKE_OP 5
#define FUTEX_WAIT_BITSET 9
#define FUTEX_WAKE_BITSET 10
#define FUTEX_PRIVATE 128
#define FUTEX_CLOCK_REALTIME 256

struct waiter {
    struct waiter *next;
    uint64_t key;
    uint32_t bitset;
    volatile int woken;
};

static spinlock_t flock = SPINLOCK_INIT("futex");
static struct waiter *waiters;

static int wake_key(uint64_t key, int count, uint32_t bitset)
{
    int n = 0;
    for (struct waiter **pp = &waiters; *pp && n < count;) {
        struct waiter *w = *pp;
        if (w->key == key && (w->bitset & bitset)) {
            *pp = w->next;
            w->woken = 1;
            sched_wake(w);
            n++;
        } else {
            pp = &w->next;
        }
    }
    return n;
}

static void remove_waiter(struct waiter *w)
{
    for (struct waiter **pp = &waiters; *pp; pp = &(*pp)->next)
        if (*pp == w) { *pp = w->next; return; }
}

void sv_futex_wake_addr(struct mm *m, uint64_t uaddr, int count)
{
    uint64_t pa = mm_user_phys(m, uaddr, false);
    if (!pa) return;
    spin_lock(&flock);
    wake_key(pa, count, ~0u);
    spin_unlock(&flock);
}

long sv_futex(uint64_t uaddr, int op, uint32_t val, uint64_t timeout, uint64_t uaddr2, uint32_t val3)
{
    struct mm *m = sv_curgrp()->mm;
    int cmd = op & 127;
    if (uaddr & 3) return -ERR_INVAL;
    uint64_t pa = mm_user_phys(m, uaddr, false);
    if (!pa) return -ERR_FAULT;
    switch (cmd) {
    case FUTEX_WAIT:
    case FUTEX_WAIT_BITSET: {
        uint64_t ms = 0;
        if (timeout) {
            struct { int64_t s, ns; } ts;
            if (copy_from_user(&ts, timeout, sizeof(ts))) return -ERR_FAULT;
            uint64_t want = (uint64_t)ts.s * 1000000000ull + (uint64_t)ts.ns;
            if (cmd == FUTEX_WAIT_BITSET) {                         /* an absolute time */
                uint64_t now = (op & FUTEX_CLOCK_REALTIME) ? sv_realtime_ns() : sv_now_ns();
                want = want > now ? want - now : 0;
                if (!want) return -ERR_TIMEDOUT;
            }
            ms = (want + 999999) / 1000000;
            if (!ms) ms = 1;
        }
        struct waiter w;
        w.key = pa;
        w.bitset = cmd == FUTEX_WAIT_BITSET ? (val3 ? val3 : ~0u) : ~0u;
        w.woken = 0;
        spin_lock(&flock);
        /* the check and the enqueue are one step with respect to wakers */
        uint32_t cur = *(volatile uint32_t *)P2V(pa);
        if (cur != val) { spin_unlock(&flock); return -ERR_AGAIN; }
        w.next = waiters;
        waiters = &w;
        long rc = 0;
        while (!w.woken) {
            int r = sv_wait_chan(&w, &flock, ms);
            if (w.woken) break;
            if (r < 0) { rc = r; break; }
            if (r == 1) { rc = -ERR_TIMEDOUT; break; }
        }
        if (!w.woken) remove_waiter(&w);
        spin_unlock(&flock);
        return rc;
    }
    case FUTEX_WAKE:
    case FUTEX_WAKE_BITSET: {
        spin_lock(&flock);
        int n = wake_key(pa, (int)val, cmd == FUTEX_WAKE_BITSET ? (val3 ? val3 : ~0u) : ~0u);
        spin_unlock(&flock);
        return n;
    }
    case FUTEX_REQUEUE:
    case FUTEX_CMP_REQUEUE: {
        uint64_t pa2 = mm_user_phys(m, uaddr2, false);
        if (!pa2) return -ERR_FAULT;
        spin_lock(&flock);
        if (cmd == FUTEX_CMP_REQUEUE && *(volatile uint32_t *)P2V(pa) != val3) { spin_unlock(&flock); return -ERR_AGAIN; }
        int woken = wake_key(pa, (int)val, ~0u);
        int moved = 0;
        for (struct waiter *w = waiters; w && moved < (int)timeout; w = w->next)
            if (w->key == pa) { w->key = pa2; moved++; }
        spin_unlock(&flock);
        return woken + moved;
    }
    case FUTEX_WAKE_OP: {
        uint64_t pa2 = mm_user_phys(m, uaddr2, true);
        if (!pa2) return -ERR_FAULT;
        uint32_t enc = val3;
        int opx = (enc >> 28) & 7, cmpx = (enc >> 24) & 15;
        int oparg = (int)(enc << 8) >> 20, cmparg = (int)(enc << 20) >> 20;
        if ((enc >> 28) & 8) oparg = 1 << oparg;
        volatile uint32_t *p2 = P2V(pa2);
        spin_lock(&flock);
        uint32_t old = *p2, nv = old;
        switch (opx) {
        case 0: nv = (uint32_t)oparg; break;
        case 1: nv = old + (uint32_t)oparg; break;
        case 2: nv = old | (uint32_t)oparg; break;
        case 3: nv = old & ~(uint32_t)oparg; break;
        case 4: nv = old ^ (uint32_t)oparg; break;
        }
        *p2 = nv;
        bool cond = false;
        switch (cmpx) {
        case 0: cond = (int)old == cmparg; break;
        case 1: cond = (int)old != cmparg; break;
        case 2: cond = (int)old < cmparg; break;
        case 3: cond = (int)old <= cmparg; break;
        case 4: cond = (int)old > cmparg; break;
        case 5: cond = (int)old >= cmparg; break;
        }
        int n = wake_key(pa, (int)val, ~0u);
        if (cond) n += wake_key(pa2, (int)timeout, ~0u);
        spin_unlock(&flock);
        return n;
    }
    default:
        return -ERR_NOSYS;                                          /* priority-inheritance futexes */
    }
}

SC(s_futex) { return sv_futex(r->rdi, (int)r->rsi, (uint32_t)r->rdx, r->r10, r->r8, (uint32_t)r->r9); }

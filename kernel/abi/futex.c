/*
 * Futexes: wait and wake keyed by the address of a word of user memory. A word
 * in memory shared between processes is identified by the physical page it is
 * in; a word in private memory by (address space, address) - its physical page
 * can change under it (copy on write) while a thread sleeps on it.
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

struct fkey { uint64_t mm, addr; };           /* mm is 0 for a word in shared memory (addr is then physical) */

struct waiter {
    struct waiter *next;
    struct fkey key;
    uint32_t bitset;
    volatile int woken;
};

static spinlock_t flock = SPINLOCK_INIT("futex");
static struct waiter *waiters;

static bool key_eq(struct fkey a, struct fkey b) { return a.mm == b.mm && a.addr == b.addr; }

/* the key for a word; for shared memory this needs the page to be present (faults it in) */
static bool make_key(struct mm *m, uint64_t uaddr, struct fkey *k)
{
    if (!user_range_ok(uaddr, 4)) return false;
    if (mm_is_shared(m, uaddr)) {
        uint64_t pa = mm_user_phys(m, uaddr, false);
        if (!pa) return false;
        k->mm = 0;
        k->addr = pa;
    } else {
        if (!mm_user_phys(m, uaddr, false)) return false;                  /* touch it: the word must exist */
        k->mm = (uint64_t)(uintptr_t)m;
        k->addr = uaddr;
    }
    return true;
}

static int wake_key(struct fkey key, int count, uint32_t bitset)
{
    int n = 0;
    for (struct waiter **pp = &waiters; *pp && n < count;) {
        struct waiter *w = *pp;
        if (key_eq(w->key, key) && (w->bitset & bitset)) {
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
    struct fkey k;
    if (!make_key(m, uaddr, &k)) return;
    spin_lock(&flock);
    wake_key(k, count, ~0u);
    spin_unlock(&flock);
}

/* the current value of the word; it has to be read under the lock, from the page that is mapped right now */
static bool read_word(struct mm *m, uint64_t uaddr, uint32_t *out)
{
    for (int tries = 0; tries < 4; tries++) {
        uint64_t pa = mm_peek_phys(m, uaddr);
        if (!pa) return false;
        *out = *(volatile uint32_t *)P2V(pa);
        if (mm_peek_phys(m, uaddr) == pa) return true;               /* the page did not move while we looked */
    }
    return false;
}

long sv_futex(uint64_t uaddr, int op, uint32_t val, uint64_t timeout, uint64_t uaddr2, uint32_t val3)
{
    struct mm *m = sv_curgrp()->mm;
    int cmd = op & 127;
    if (uaddr & 3) return -ERR_INVAL;
    struct fkey key;
    if (!make_key(m, uaddr, &key)) return -ERR_FAULT;
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
        w.key = key;
        w.bitset = cmd == FUTEX_WAIT_BITSET ? (val3 ? val3 : ~0u) : ~0u;
        w.woken = 0;
        spin_lock(&flock);
        /* the check and the enqueue are one step with respect to wakers */
        uint32_t cur;
        if (!read_word(m, uaddr, &cur)) { spin_unlock(&flock); return -ERR_FAULT; }
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
        int n = wake_key(key, (int)val, cmd == FUTEX_WAKE_BITSET ? (val3 ? val3 : ~0u) : ~0u);
        spin_unlock(&flock);
        return n;
    }
    case FUTEX_REQUEUE:
    case FUTEX_CMP_REQUEUE: {
        struct fkey key2;
        if (!make_key(m, uaddr2, &key2)) return -ERR_FAULT;
        spin_lock(&flock);
        uint32_t cur;
        if (cmd == FUTEX_CMP_REQUEUE && (!read_word(m, uaddr, &cur) || cur != val3)) { spin_unlock(&flock); return -ERR_AGAIN; }
        int woken = wake_key(key, (int)val, ~0u);
        int moved = 0;
        for (struct waiter *w = waiters; w && moved < (int)timeout; w = w->next)
            if (key_eq(w->key, key)) { w->key = key2; moved++; }
        spin_unlock(&flock);
        return woken + moved;
    }
    case FUTEX_WAKE_OP: {
        struct fkey key2;
        if (!make_key(m, uaddr2, &key2)) return -ERR_FAULT;
        uint32_t enc = val3;
        int opx = (enc >> 28) & 7, cmpx = (enc >> 24) & 15;
        int oparg = (int)(enc << 8) >> 20, cmparg = (int)(enc << 20) >> 20;
        if ((enc >> 28) & 8) oparg = 1 << oparg;
        spin_lock(&flock);
        uint64_t pa2 = mm_peek_phys(m, uaddr2);
        if (!pa2) { spin_unlock(&flock); return -ERR_FAULT; }
        volatile uint32_t *p2 = P2V(pa2);
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
        int n = wake_key(key, (int)val, ~0u);
        if (cond) n += wake_key(key2, (int)timeout, ~0u);
        spin_unlock(&flock);
        return n;
    }
    default:
        return -ERR_NOSYS;                                          /* priority-inheritance futexes */
    }
}

SC(s_futex) { return sv_futex(r->rdi, (int)r->rsi, (uint32_t)r->rdx, r->r10, r->r8, (uint32_t)r->r9); }

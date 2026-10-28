/*
 * Preemptive SMP scheduler.
 *
 * One global run queue (two priority levels) protected by sched_lock.
 * Every CPU runs a LAPIC timer at 1 kHz; a thread runs for a slice of
 * SLICE_MS before it is preempted. Blocking uses wait channels
 * (sleep/wakeup on an address), with optional timeouts.
 *
 * sched_lock is held across switch_context(); the thread that is switched
 * to releases it (xv6 style), so a thread is never visible on the run
 * queue before its registers are saved.
 */
#include <sched.h>
#include <cpu.h>
#include <x86.h>
#include <dev.h>

#define SLICE_MS 6
#define DEFAULT_STACK (32 * 1024)

static spinlock_t sched_lock = SPINLOCK_INIT("sched");
static thread_t *rq_head[2], *rq_tail[2];
static thread_t *all_threads;
static thread_t *blocked;
static thread_t *zombies;
static int next_tid = 1;
static int nthreads;
static uint64_t nswitches;
static thread_t *reaper_thread;

/* ------------------------------------------------------------------------
 * run queue (sched_lock held)
 * ---------------------------------------------------------------------- */

static void rq_push(thread_t *t)
{
    int p = t->prio ? 1 : 0;
    t->next = NULL;
    if (rq_tail[p]) rq_tail[p]->next = t;
    else rq_head[p] = t;
    rq_tail[p] = t;
}

static thread_t *rq_pop(int cpu)
{
    for (int p = 1; p >= 0; p--) {
        thread_t *prev = NULL;
        for (thread_t *t = rq_head[p]; t; prev = t, t = t->next) {
            if (t->affinity >= 0 && t->affinity != cpu) continue;
            if (prev) prev->next = t->next;
            else rq_head[p] = t->next;
            if (rq_tail[p] == t) rq_tail[p] = prev;
            t->next = NULL;
            return t;
        }
    }
    return NULL;
}

static bool rq_has_work(int cpu)
{
    for (int p = 1; p >= 0; p--)
        for (thread_t *t = rq_head[p]; t; t = t->next)
            if (t->affinity < 0 || t->affinity == cpu) return true;
    return false;
}

static void blocked_remove(thread_t *t)
{
    thread_t **pp = &blocked;
    while (*pp) {
        if (*pp == t) { *pp = t->blk_next; t->blk_next = NULL; return; }
        pp = &(*pp)->blk_next;
    }
}

/* ------------------------------------------------------------------------
 * core switch (sched_lock held, interrupts off)
 * ---------------------------------------------------------------------- */

static void schedule(void)
{
    struct cpu *c = this_cpu();
    thread_t *prev = c->current;
    thread_t *next = rq_pop(c->id);

    if (!next) {
        if (prev->state == T_RUNNING) return;   /* nothing better to do */
        next = c->idle;
    }
    if (prev->state == T_RUNNING && prev != c->idle) {
        prev->state = T_READY;
        rq_push(prev);
    }
    if (next == prev) {
        prev->state = T_RUNNING;
        return;
    }

    next->state = T_RUNNING;
    next->last_cpu = c->id;
    c->current = next;
    c->slice_start = timer_ticks;
    set_kernel_stack((uint64_t)next->kstack + next->kstack_size);
    addrspace_t *nas = next->proc ? next->proc->as : &kernel_space;
    c->active_as = nas;
    vmm_switch(nas);
    if (next->proc && next->proc->sv) wrmsr(MSR_FS_BASE, next->fs_base);          /* each thread has its own TLS pointer */
    else if (prev->proc && prev->proc->sv) wrmsr(MSR_FS_BASE, 0);
    nswitches++;

    int intena = c->intena;
    switch_context(&prev->rsp, next->rsp);
    this_cpu()->intena = intena;
}

/* Called by thread_trampoline on a brand new thread. */
void sched_thread_started(void)
{
    this_cpu()->intena = 1;
    spin_unlock(&sched_lock);
}

/* ------------------------------------------------------------------------
 * thread creation / exit
 * ---------------------------------------------------------------------- */

thread_t *thread_create_ex(const char *name, int (*fn)(void *), void *arg, int prio, int affinity,
                           struct process *proc, size_t stack_size)
{
    thread_t *t = kzalloc(sizeof(*t));
    if (!t) return NULL;
    if (!stack_size) stack_size = DEFAULT_STACK;
    t->kstack = kmalloc(stack_size);
    if (!t->kstack) { kfree(t); return NULL; }
    t->kstack_size = stack_size;
    strlcpy(t->name, name, sizeof(t->name));
    t->prio = prio;
    t->affinity = affinity;
    t->proc = proc;
    t->last_cpu = -1;
    t->created_at = timer_ticks;

    /* initial frame consumed by switch_context: r15 r14 r13 r12 rbx rbp ret */
    uint64_t *sp = (uint64_t *)(t->kstack + stack_size);
    *--sp = 0;                              /* alignment / fake return */
    *--sp = (uint64_t)thread_trampoline;
    *--sp = 0;                              /* rbp */
    *--sp = 0;                              /* rbx */
    *--sp = (uint64_t)fn;                   /* r12 */
    *--sp = (uint64_t)arg;                  /* r13 */
    *--sp = 0;                              /* r14 */
    *--sp = 0;                              /* r15 */
    t->rsp = (uint64_t)sp;

    spin_lock(&sched_lock);
    t->tid = next_tid++;
    t->all_next = all_threads;
    all_threads = t;
    nthreads++;
    t->state = T_READY;
    rq_push(t);
    spin_unlock(&sched_lock);
    return t;
}

thread_t *thread_create(const char *name, int (*fn)(void *), void *arg)
{
    return thread_create_ex(name, fn, arg, 0, -1, NULL, 0);
}

thread_t *thread_current(void)
{
    push_cli();
    thread_t *t = this_cpu()->current;
    pop_cli();
    return t;
}

void sv_task_exit_hook(thread_t *th);
void sv_task_reap(thread_t *th);

void thread_exit(int code)
{
    thread_t *t = thread_current();
    if (t->task) sv_task_exit_hook(t);                  /* a thread of a System V program */
    else if (t->proc && !t->proc->exited) proc_exit(code);   /* cleans up, then comes back here */
    spin_lock(&sched_lock);
    t->exit_code = code;
    t->state = T_DEAD;
    t->blk_next = zombies;
    zombies = t;
    if (reaper_thread && reaper_thread->state == T_BLOCKED) {
        blocked_remove(reaper_thread);
        reaper_thread->state = T_READY;
        rq_push(reaper_thread);
    }
    schedule();
    panic("thread_exit: dead thread scheduled");
}

/* Frees dead threads once they are guaranteed to be off their stacks. */
static int reaper_main(void *arg)
{
    UNUSED(arg);
    for (;;) {
        spin_lock(&sched_lock);
        while (!zombies) {
            thread_t *me = this_cpu()->current;
            me->state = T_BLOCKED;
            me->wait_chan = &zombies;
            me->wake_at = 0;
            me->blk_next = blocked;
            blocked = me;
            schedule();
        }
        thread_t *z = zombies;
        zombies = z->blk_next;
        /* unlink from the all-threads list */
        thread_t **pp = &all_threads;
        while (*pp && *pp != z) pp = &(*pp)->all_next;
        if (*pp) *pp = z->all_next;
        nthreads--;
        spin_unlock(&sched_lock);

        if (z->task) sv_task_reap(z);
        else if (z->proc) proc_reap(z);
        kfree(z->kstack);
        kfree(z);
    }
    return 0;
}

/* ------------------------------------------------------------------------
 * blocking
 * ---------------------------------------------------------------------- */

void sched_yield(void)
{
    spin_lock(&sched_lock);
    schedule();
    spin_unlock(&sched_lock);
}

int sched_wait(void *chan, spinlock_t *lk, uint64_t timeout_ms)
{
    spin_lock(&sched_lock);
    if (lk) spin_unlock(lk);
    thread_t *t = this_cpu()->current;
    if (t->task && t->interrupted) {                   /* a signal came in just before: do not go to sleep */
        spin_unlock(&sched_lock);
        if (lk) spin_lock(lk);
        return 0;
    }
    t->wait_chan = chan;
    t->wake_at = timeout_ms ? timer_ticks + timeout_ms : 0;
    t->timed_out = 0;
    t->state = T_BLOCKED;
    t->blk_next = blocked;
    blocked = t;
    schedule();
    int r = t->timed_out;
    spin_unlock(&sched_lock);
    if (lk) spin_lock(lk);
    return r;
}

void sched_sleep(uint64_t ms)
{
    if (!ms) { sched_yield(); return; }
    static char sleep_chan;
    sched_wait(&sleep_chan, NULL, ms);
}

static void make_ready(thread_t *t)
{
    t->state = T_READY;
    t->wait_chan = NULL;
    rq_push(t);
    struct cpu *c = this_cpu();
    if (c->current == c->idle) c->need_resched = 1;
}

/* wake a specific thread out of a wait (a signal arrived for it) */
void sched_interrupt(thread_t *t)
{
    spin_lock(&sched_lock);
    if (t->state == T_BLOCKED) {
        blocked_remove(t);
        t->interrupted = 1;
        make_ready(t);
    } else {
        t->interrupted = 1;                    /* it is not asleep yet: its next wait returns at once */
    }
    spin_unlock(&sched_lock);
}

void sched_wake(void *chan)
{
    spin_lock(&sched_lock);
    thread_t **pp = &blocked;
    while (*pp) {
        thread_t *t = *pp;
        if (t->wait_chan == chan) {
            *pp = t->blk_next;
            t->blk_next = NULL;
            make_ready(t);
        } else {
            pp = &t->blk_next;
        }
    }
    spin_unlock(&sched_lock);
}

/* ------------------------------------------------------------------------
 * timer tick (every CPU, 1 kHz)
 * ---------------------------------------------------------------------- */

static uint64_t last_stats;

void sched_tick(void)
{
    struct cpu *c = this_cpu();
    thread_t *cur = c->current;
    if (cur == c->idle) c->ticks_idle++;
    else c->ticks_busy++;
    if (cur) cur->ticks++;

    spin_lock(&sched_lock);
    if (c->id == 0) {
        uint64_t now = timer_ticks;
        thread_t **pp = &blocked;
        while (*pp) {
            thread_t *t = *pp;
            if (t->wake_at && t->wake_at <= now) {
                *pp = t->blk_next;
                t->blk_next = NULL;
                t->timed_out = 1;
                make_ready(t);
            } else {
                pp = &t->blk_next;
            }
        }
        /* per-thread and per-CPU load, twice a second */
        if (now - last_stats >= 500) {
            uint64_t span = now - last_stats;
            last_stats = now;
            for (thread_t *t = all_threads; t; t = t->all_next) {
                uint64_t d = t->ticks - t->ticks_mark;
                t->ticks_mark = t->ticks;
                t->cpu_pct = (uint32_t)(d * 1000 / span);
            }
            for (int i = 0; i < ncpus; i++) {
                struct cpu *x = &cpus[i];
                uint64_t b = x->ticks_busy - x->hist_busy, id = x->ticks_idle - x->hist_idle;
                x->hist_busy = x->ticks_busy;
                x->hist_idle = x->ticks_idle;
                x->load = b + id ? (uint32_t)(b * 100 / (b + id)) : 0;
            }
        }
    }
    if (cur == c->idle) {
        if (rq_has_work(c->id)) c->need_resched = 1;
    } else if (timer_ticks - c->slice_start >= SLICE_MS && rq_has_work(c->id)) {
        c->need_resched = 1;
    }
    spin_unlock(&sched_lock);
}

void sched_preempt(void)
{
    struct cpu *c = this_cpu();
    c->need_resched = 0;
    spin_lock(&sched_lock);
    schedule();
    spin_unlock(&sched_lock);
}

/* ------------------------------------------------------------------------
 * introspection
 * ---------------------------------------------------------------------- */

int sched_snapshot(struct thread_snapshot *out, int max)
{
    int n = 0;
    spin_lock(&sched_lock);
    for (thread_t *t = all_threads; t && n < max; t = t->all_next) {
        struct thread_snapshot *s = &out[n++];
        s->tid = t->tid;
        s->pid = t->proc ? t->proc->pid : 0;
        strlcpy(s->name, t->name, sizeof(s->name));
        s->state = t->state;
        s->cpu = t->last_cpu;
        s->cpu_pct = t->cpu_pct;
        s->ticks = t->ticks;
        s->user = t->proc != NULL;
        s->mem = t->proc ? t->proc->mem_pages * PAGE_SIZE : t->kstack_size;
    }
    spin_unlock(&sched_lock);
    return n;
}

int sched_thread_count(void) { return nthreads; }
uint64_t sched_context_switches(void) { return nswitches; }

/* ------------------------------------------------------------------------
 * init
 * ---------------------------------------------------------------------- */

static int idle_main(void *arg)
{
    UNUSED(arg);
    for (;;) {
        sti();
        hlt();
    }
    return 0;
}

static thread_t *make_idle(int cpu)
{
    thread_t *t = kzalloc(sizeof(*t));
    t->kstack_size = 8192;
    t->kstack = kmalloc(t->kstack_size);
    snprintf(t->name, sizeof(t->name), "idle/%d", cpu);
    t->affinity = cpu;
    t->last_cpu = cpu;
    uint64_t *sp = (uint64_t *)(t->kstack + t->kstack_size);
    *--sp = 0;
    *--sp = (uint64_t)thread_trampoline;
    *--sp = 0;
    *--sp = 0;
    *--sp = (uint64_t)idle_main;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    t->rsp = (uint64_t)sp;
    t->state = T_READY;
    t->tid = 0;
    return t;
}

void sched_init(void)
{
    struct cpu *c = this_cpu();
    /* the boot flow becomes the "kmain" thread */
    thread_t *t = kzalloc(sizeof(*t));
    strlcpy(t->name, "kmain", sizeof(t->name));
    t->state = T_RUNNING;
    t->affinity = -1;
    t->last_cpu = 0;
    extern char boot_stack[];
    t->kstack = (uint8_t *)boot_stack;
    t->kstack_size = 64 * 1024;
    t->tid = next_tid++;
    t->all_next = all_threads;
    all_threads = t;
    nthreads++;
    c->current = t;
    c->idle = make_idle(0);
    set_kernel_stack((uint64_t)t->kstack + t->kstack_size);
    reaper_thread = thread_create("reaper", reaper_main, NULL);
}

/* An AP enters here after its own setup and becomes its idle thread. */
void sched_start_ap(void)
{
    struct cpu *c = this_cpu();
    c->idle = make_idle(c->id);
    c->current = c->idle;
    c->idle->state = T_RUNNING;
    set_kernel_stack((uint64_t)c->idle->kstack + c->idle->kstack_size);
    /* jump onto the idle thread's own stack via a throwaway context */
    uint64_t dummy;
    spin_lock(&sched_lock);
    switch_context(&dummy, c->idle->rsp);
    panic("sched_start_ap returned");
}

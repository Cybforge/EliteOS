/*
 * Signals: actions, masks, delivery to handlers (with the stack frame layout
 * handlers and signal-return code expect), default actions, faults turned into
 * signals.
 */
#include <kernel.h>
#include <cpu.h>
#include <x86.h>
#include <sched.h>
#include <mm.h>
#include <umm.h>
#include <abierr.h>
#include "sv.h"

extern spinlock_t sv_lock;

struct k_stack { uint64_t ss_sp; int32_t ss_flags; int32_t pad; uint64_t ss_size; };

struct k_sigcontext {
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15, rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp, rip, eflags;
    uint16_t cs, gs, fs, ss;
    uint64_t err, trapno, oldmask, cr2;
    uint64_t fpstate;
    uint64_t reserved[8];
};

struct k_ucontext {
    uint64_t uc_flags;
    uint64_t uc_link;
    struct k_stack uc_stack;
    struct k_sigcontext uc_mcontext;
    uint64_t uc_sigmask;
};

struct k_siginfo {
    int32_t si_signo, si_errno, si_code, pad;
    union {
        struct { int32_t pid; uint32_t uid; int32_t status; } kill;
        struct { uint64_t addr; } fault;
        uint8_t raw[112];
    } u;
};

struct sigframe {
    uint64_t pretcode;
    struct k_ucontext uc;
    struct k_siginfo info;
};

#define SI_USER 0
#define SI_KERNEL 0x80
#define SI_TKILL (-6)
#define SEGV_MAPERR 1
#define SEGV_ACCERR 2

static const sigset64_t unblockable = SIGBIT(SIGKILL) | SIGBIT(SIGSTOP);
/* signals whose default is to be ignored */
static const sigset64_t default_ignored = SIGBIT(SIGCHLD) | SIGBIT(SIGURG) | SIGBIT(SIGWINCH) | SIGBIT(SIGCONT) | SIGBIT(SIGSTOP) |
                                          SIGBIT(SIGTSTP) | SIGBIT(SIGTTIN) | SIGBIT(SIGTTOU);

struct sv_task *sv_cur(void) { return thread_current()->task; }
struct sv_group *sv_curgrp(void) { struct sv_task *t = sv_cur(); return t ? t->g : NULL; }

static bool ignored_now(struct sv_group *g, int sig)
{
    uint64_t h = g->sig->act[sig].handler;
    return h == SIG_IGN || (h == SIG_DFL && (default_ignored & SIGBIT(sig)));
}

bool sv_signal_pending(struct sv_task *t)
{
    return ((t->pending | t->g->pending) & ~t->blocked & ~0ull) != 0 || t->exiting;
}

void sv_wake_task(struct sv_task *t)
{
    extern void sched_interrupt(thread_t *);
    if (t->thr) sched_interrupt(t->thr);
}

/* ------------------------------------------------------------------------
 * sending
 * ---------------------------------------------------------------------- */

static void kill_group_locked(struct sv_group *g)
{
    for (struct sv_task *t = g->tasks; t; t = t->next) {
        t->exiting = true;
        sv_wake_task(t);
    }
}

void sv_send_signal(struct sv_group *g, struct sv_task *t, int sig)
{
    if (sig <= 0 || sig > NSIG || !g || g->zombie) return;
    spin_lock(&sv_lock);
    if (sig == SIGKILL) {
        g->exit_code = SIGKILL;                          /* the status the parent will see */
        kill_group_locked(g);
        spin_unlock(&sv_lock);
        return;
    }
    if (ignored_now(g, sig) && sig != SIGSTOP) { spin_unlock(&sv_lock); return; }
    if (t) t->pending |= SIGBIT(sig);
    else g->pending |= SIGBIT(sig);
    /* wake a thread that will take it */
    struct sv_task *target = t;
    if (!target)
        for (struct sv_task *x = g->tasks; x; x = x->next)
            if (!(x->blocked & SIGBIT(sig))) { target = x; break; }
    spin_unlock(&sv_lock);
    if (target) sv_wake_task(target);
    sv_poll_wake();                                      /* a signalfd may be waiting for it */
}

int sv_kill_group(int tgid, int sig)
{
    if (sig < 0 || sig > NSIG) return -ERR_INVAL;
    struct sv_group *g = sv_find_group(tgid);
    if (!g) return -ERR_SRCH;
    if (sig) sv_send_signal(g, NULL, sig);
    return 0;
}

int sv_kill_task(int tgid, int tid, int sig)
{
    if (sig < 0 || sig > NSIG) return -ERR_INVAL;
    struct sv_task *t = sv_find_task(tid);
    if (!t || (tgid > 0 && t->g->tgid != tgid)) return -ERR_SRCH;
    if (sig) sv_send_signal(t->g, t, sig);
    return 0;
}

/* ------------------------------------------------------------------------
 * system calls about actions and masks
 * ---------------------------------------------------------------------- */

long sv_sigaction(int sig, uint64_t act, uint64_t old, size_t size)
{
    struct sv_task *t = sv_cur();
    if (size != 8) return -ERR_INVAL;
    if (sig < 1 || sig > NSIG) return -ERR_INVAL;
    struct sv_sigaction na;
    if (act) {
        if (copy_from_user(&na, act, sizeof(na))) return -ERR_FAULT;
        if (sig == SIGKILL || sig == SIGSTOP) return -ERR_INVAL;
        na.mask &= ~unblockable;
    }
    spin_lock(&sv_lock);
    struct sv_sigaction prev = t->g->sig->act[sig];
    if (act) {
        t->g->sig->act[sig] = na;
        /* a signal that is now ignored is dropped from what is pending */
        if (ignored_now(t->g, sig)) { t->g->pending &= ~SIGBIT(sig); for (struct sv_task *x = t->g->tasks; x; x = x->next) x->pending &= ~SIGBIT(sig); }
    }
    spin_unlock(&sv_lock);
    if (old && copy_to_user(old, &prev, sizeof(prev))) return -ERR_FAULT;
    return 0;
}

long sv_sigprocmask(int how, uint64_t set, uint64_t old, size_t size)
{
    struct sv_task *t = sv_cur();
    if (size != 8) return -ERR_INVAL;
    sigset64_t cur = t->blocked, ns = 0;
    if (set && copy_from_user(&ns, set, 8)) return -ERR_FAULT;
    if (old && copy_to_user(old, &cur, 8)) return -ERR_FAULT;
    if (set) {
        switch (how) {
        case 0: cur |= ns; break;                       /* SIG_BLOCK */
        case 1: cur &= ~ns; break;                      /* SIG_UNBLOCK */
        case 2: cur = ns; break;                        /* SIG_SETMASK */
        default: return -ERR_INVAL;
        }
        t->blocked = cur & ~unblockable;
    }
    return 0;
}

long sv_sigaltstack(uint64_t ss, uint64_t old)
{
    struct sv_task *t = sv_cur();
    struct k_stack cur = { t->altstack_sp, (int32_t)t->altstack_flags, 0, t->altstack_size };
    if (old && copy_to_user(old, &cur, sizeof(cur))) return -ERR_FAULT;
    if (ss) {
        struct k_stack n;
        if (copy_from_user(&n, ss, sizeof(n))) return -ERR_FAULT;
        if (n.ss_flags & ~(2 | 0x80000000)) return -ERR_INVAL;
        if (n.ss_flags & 2) { t->altstack_sp = 0; t->altstack_size = 0; t->altstack_flags = 2; }   /* SS_DISABLE */
        else {
            if (n.ss_size < 2048) return -ERR_NOMEM;
            t->altstack_sp = n.ss_sp;
            t->altstack_size = n.ss_size;
            t->altstack_flags = 0;
        }
    }
    return 0;
}

long sv_sigpending(uint64_t set, size_t size)
{
    struct sv_task *t = sv_cur();
    if (size != 8) return -ERR_INVAL;
    sigset64_t p = (t->pending | t->g->pending) & t->blocked;
    return copy_to_user(set, &p, 8) ? -ERR_FAULT : 0;
}

/* sigsuspend: replace the mask and sleep until a handler has run */
long sv_sigsuspend(uint64_t mask, size_t size)
{
    struct sv_task *t = sv_cur();
    if (size != 8) return -ERR_INVAL;
    sigset64_t m;
    if (copy_from_user(&m, mask, 8)) return -ERR_FAULT;
    t->saved_mask = t->blocked;
    t->has_saved_mask = true;
    t->blocked = m & ~unblockable;
    for (;;) {
        spin_lock(&sv_lock);
        if (sv_signal_pending(t)) { spin_unlock(&sv_lock); break; }
        sched_wait(t, &sv_lock, 0);
        spin_unlock(&sv_lock);
    }
    return -ERR_INTR;
}

/* ------------------------------------------------------------------------
 * delivery
 * ---------------------------------------------------------------------- */

static void fill_context(struct k_sigcontext *sc, const struct regs *r, uint64_t fpptr, uint64_t cr2, uint64_t mask, int trapno, uint64_t err)
{
    memset(sc, 0, sizeof(*sc));
    sc->r8 = r->r8; sc->r9 = r->r9; sc->r10 = r->r10; sc->r11 = r->r11; sc->r12 = r->r12; sc->r13 = r->r13;
    sc->r14 = r->r14; sc->r15 = r->r15; sc->rdi = r->rdi; sc->rsi = r->rsi; sc->rbp = r->rbp; sc->rbx = r->rbx;
    sc->rdx = r->rdx; sc->rax = r->rax; sc->rcx = r->rcx; sc->rsp = r->rsp; sc->rip = r->rip; sc->eflags = r->rflags;
    sc->cs = 0x2B; sc->ss = 0x23;
    sc->err = err;
    sc->trapno = (uint64_t)trapno;
    sc->oldmask = mask;
    sc->cr2 = cr2;
    sc->fpstate = fpptr;
}

static void default_fx(uint8_t *fx)
{
    memset(fx, 0, 512);
    *(uint16_t *)fx = 0x37F;                      /* x87 control word */
    *(uint32_t *)(fx + 24) = 0x1F80;              /* MXCSR */
}

NORETURN static void die_by_signal(int sig)
{
    sv_exit_task(sig & 0x7F, true);
}

/* build a frame for the handler; false if the stack is unusable */
static bool setup_frame(struct sv_task *t, struct regs *r, int sig, const struct sv_sigaction *act, const struct k_siginfo *info)
{
    uint8_t *fx = (uint8_t *)r - 512;
    uint64_t sp = r->rsp;
    bool on_alt = t->altstack_size && sp >= t->altstack_sp && sp < t->altstack_sp + t->altstack_size;
    if ((act->flags & SA_ONSTACK) && t->altstack_size && !on_alt) sp = t->altstack_sp + t->altstack_size;
    else sp -= 128;                                                 /* the red zone below the stack pointer is the program's */
    sp -= 512;
    sp = ALIGN_DOWN(sp, 64);
    uint64_t fpptr = sp;
    sp -= sizeof(struct sigframe);
    sp = ALIGN_DOWN(sp, 16) - 8;                                    /* (sp + 8) is 16-byte aligned when the handler starts */

    struct sigframe f;
    memset(&f, 0, sizeof(f));
    if (!(act->flags & SA_RESTORER)) return false;
    f.pretcode = act->restorer;
    f.uc.uc_flags = 0;
    f.uc.uc_stack.ss_sp = t->altstack_sp;
    f.uc.uc_stack.ss_flags = on_alt ? 1 : (int32_t)t->altstack_flags;      /* SS_ONSTACK */
    f.uc.uc_stack.ss_size = t->altstack_size;
    uint64_t cr2 = (info->si_signo == SIGSEGV || info->si_signo == SIGBUS) ? info->u.fault.addr : 0;
    fill_context(&f.uc.uc_mcontext, r, fpptr, cr2, t->blocked, 0, 0);
    f.uc.uc_sigmask = t->blocked;
    f.info = *info;
    if (copy_to_user(sp, &f, sizeof(f)) || copy_to_user(fpptr, fx, 512)) return false;

    r->rip = act->handler;
    r->rsp = sp;
    r->rdi = (uint64_t)sig;
    r->rsi = sp + offsetof(struct sigframe, info);
    r->rdx = sp + offsetof(struct sigframe, uc);
    r->rax = 0;
    r->rflags &= ~(uint64_t)((1 << 8) | (1 << 10));                 /* TF, DF */
    default_fx(fx);
    return true;
}

long sv_rt_sigreturn(struct regs *r)
{
    struct sv_task *t = sv_cur();
    struct k_ucontext uc;
    if (copy_from_user(&uc, r->rsp, sizeof(uc))) die_by_signal(SIGSEGV);
    struct k_sigcontext *sc = &uc.uc_mcontext;
    r->r8 = sc->r8; r->r9 = sc->r9; r->r10 = sc->r10; r->r11 = sc->r11; r->r12 = sc->r12; r->r13 = sc->r13;
    r->r14 = sc->r14; r->r15 = sc->r15; r->rdi = sc->rdi; r->rsi = sc->rsi; r->rbp = sc->rbp; r->rbx = sc->rbx;
    r->rdx = sc->rdx; r->rcx = sc->rcx; r->rsp = sc->rsp; r->rip = sc->rip;
    r->rflags = (sc->eflags & 0x254DD5) | 0x202;                    /* only the flags a program may set; interrupts stay on */
    if (!user_range_ok(r->rip, 1)) die_by_signal(SIGSEGV);          /* a non-canonical return address must never reach SYSRET */
    t->blocked = uc.uc_sigmask & ~unblockable;
    if (sc->fpstate) {
        uint8_t fx[512] __attribute__((aligned(16)));
        if (copy_from_user(fx, sc->fpstate, 512) == 0) memcpy((uint8_t *)r - 512, fx, 512);
    }
    r->vector = 0x81;                                               /* leave through IRET: SYSRET would clobber rcx and r11 */
    r->cs = 0x2B;
    r->ss = 0x23;
    return (long)sc->rax;
}

/* Run on the way back to user mode: take pending signals. */
static void deliver(struct sv_task *t, struct regs *r)
{
    struct sv_group *g = t->g;
    for (;;) {
        spin_lock(&sv_lock);
        sigset64_t avail = (t->pending | g->pending) & ~t->blocked;
        if (!avail) { spin_unlock(&sv_lock); return; }
        int sig = __builtin_ctzll(avail) + 1;
        if (t->pending & SIGBIT(sig)) t->pending &= ~SIGBIT(sig);
        else g->pending &= ~SIGBIT(sig);
        struct sv_sigaction act = g->sig->act[sig];
        spin_unlock(&sv_lock);

        if (act.handler == SIG_IGN) continue;
        if (act.handler == SIG_DFL) {
            if (default_ignored & SIGBIT(sig)) continue;
            die_by_signal(sig);
        }
        struct k_siginfo info;
        memset(&info, 0, sizeof(info));
        info.si_signo = sig;
        info.si_code = SI_USER;
        info.u.kill.pid = 0;
        info.u.kill.uid = g->uid;
        if (sig == t->tgkill_sig) { info.si_code = SI_TKILL; t->tgkill_sig = 0; }
        if (t->sigsegv_code && (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGFPE || sig == SIGTRAP)) {
            info.si_code = t->sigsegv_code;
            info.u.fault.addr = t->sigsegv_addr;
            t->sigsegv_code = 0;
        }
        /* restart an interrupted system call if the handler asked for it */
        if ((long)r->rax == -ERR_INTR && t->syscall_nr >= 0) {
            if (act.flags & SA_RESTART) { r->rip -= 2; r->rax = (uint64_t)t->syscall_nr; }
        }
        t->syscall_nr = -1;
        if (!setup_frame(t, r, sig, &act, &info)) die_by_signal(SIGSEGV);
        spin_lock(&sv_lock);
        t->blocked |= act.mask | ((act.flags & SA_NODEFER) ? 0 : SIGBIT(sig));
        t->blocked &= ~unblockable;
        if (act.flags & SA_RESETHAND) g->sig->act[sig].handler = SIG_DFL;
        spin_unlock(&sv_lock);
        return;
    }
}

void sv_return_to_user(struct regs *r)
{
    struct sv_task *t = sv_cur();
    if (!t) return;
    if (t->exiting || t->g->proc->killed) sv_exit_task(t->g->exit_code ? t->g->exit_code : 0, false);
    if (t->has_saved_mask && sv_signal_pending(t) == false) { /* nothing */ }
    deliver(t, r);
    if (t->has_saved_mask) { t->blocked = t->saved_mask; t->has_saved_mask = false; }
    t->syscall_nr = -1;
}

/* A fault in user mode: a missing page is made, anything else becomes a signal. */
void sv_fault(struct regs *r)
{
    struct sv_task *t = sv_cur();
    uint64_t v = r->vector;
    sti();
    if (v == 14) {
        uint64_t addr = thread_current()->fault_addr;
        if (mm_fault(t->g->mm, addr, r->error)) return;
        mm_fault_report(t->g->mm, addr, r->error);
        t->sigsegv_addr = addr;
        t->sigsegv_code = (r->error & 1) ? SEGV_ACCERR : SEGV_MAPERR;
        int sig = SIGSEGV;
        if (t->g->sig->act[sig].handler == SIG_DFL)
            klog("abi: %s (pid %d) fault at rip %#lx accessing %#lx (error %#lx, rsp %#lx)", t->g->comm, t->g->tgid, r->rip, addr,
                 r->error, r->rsp);
        spin_lock(&sv_lock);
        if (t->blocked & SIGBIT(sig)) { t->blocked &= ~SIGBIT(sig); t->g->sig->act[sig].handler = SIG_DFL; }    /* a blocked fault kills */
        t->pending |= SIGBIT(sig);
        spin_unlock(&sv_lock);
        return;
    }
    int sig = SIGSEGV, code = SI_KERNEL;
    switch (v) {
    case 0: case 16: case 19: sig = SIGFPE; code = 1; break;
    case 1: case 3: sig = SIGTRAP; code = 1; break;
    case 6: sig = SIGILL; code = 2; break;
    case 17: sig = SIGBUS; code = 1; break;
    default: break;
    }
    t->sigsegv_addr = r->rip;
    t->sigsegv_code = code;
    if (t->g->sig->act[sig].handler == SIG_DFL)
        klog("abi: %s (pid %d) exception %lu at rip %#lx (error %#lx)", t->g->comm, t->g->tgid, v, r->rip, r->error);
    spin_lock(&sv_lock);
    if ((t->blocked & SIGBIT(sig)) || t->g->sig->act[sig].handler == SIG_IGN) { t->blocked &= ~SIGBIT(sig); t->g->sig->act[sig].handler = SIG_DFL; }
    t->pending |= SIGBIT(sig);
    spin_unlock(&sv_lock);
}

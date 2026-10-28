/*
 * Processes and threads of the System V interface: creating (spawn, fork,
 * clone), replacing (exec), ending (exit, fatal signals) and waiting.
 */
#include <kernel.h>
#include <cpu.h>
#include <x86.h>
#include <sched.h>
#include <mm.h>
#include <dev.h>
#include <tty.h>
#include <sys.h>
#include <umm.h>
#include <abierr.h>
#include "sv.h"

spinlock_t sv_lock = SPINLOCK_INIT("sv");
struct sv_group *sv_groups;
static int next_id = 1000;

extern NORETURN void enter_user_regs(struct regs *r, const void *fx);
void proc_register(process_t *p);
void proc_unregister(process_t *p);

#define CLONE_VM 0x100u
#define CLONE_FS 0x200u
#define CLONE_FILES 0x400u
#define CLONE_SIGHAND 0x800u
#define CLONE_VFORK 0x4000u
#define CLONE_PARENT 0x8000u
#define CLONE_THREAD 0x10000u
#define CLONE_SETTLS 0x80000u
#define CLONE_PARENT_SETTID 0x100000u
#define CLONE_CHILD_CLEARTID 0x200000u
#define CLONE_CHILD_SETTID 0x1000000u
#define CLONE_NS_MASK 0x7E020000u            /* the namespace flags */

/* ------------------------------------------------------------------------
 * lookup
 * ---------------------------------------------------------------------- */

struct sv_group *sv_find_group(int tgid)
{
    struct sv_group *r = NULL;
    spin_lock(&sv_lock);
    for (struct sv_group *g = sv_groups; g; g = g->next_all)
        if (g->tgid == tgid && !g->reaped) { r = g; break; }
    spin_unlock(&sv_lock);
    return r;
}

struct sv_task *sv_find_task(int tid)
{
    struct sv_task *r = NULL;
    spin_lock(&sv_lock);
    for (struct sv_group *g = sv_groups; g && !r; g = g->next_all)
        for (struct sv_task *t = g->tasks; t; t = t->next)
            if (t->tid == tid) { r = t; break; }
    spin_unlock(&sv_lock);
    return r;
}

int sv_group_list(int *ids, int max)
{
    int n = 0;
    spin_lock(&sv_lock);
    for (struct sv_group *g = sv_groups; g && n < max; g = g->next_all)
        if (!g->zombie) ids[n++] = g->tgid;
    spin_unlock(&sv_lock);
    return n;
}

/* ------------------------------------------------------------------------
 * creation
 * ---------------------------------------------------------------------- */

static struct sv_sighand *sighand_new(void)
{
    struct sv_sighand *h = kzalloc(sizeof(*h));
    if (h) h->refs = 1;
    return h;
}

static void sighand_put(struct sv_sighand *h)
{
    if (h && __atomic_sub_fetch(&h->refs, 1, __ATOMIC_ACQ_REL) == 0) kfree(h);
}

static void rlimits_default(struct sv_group *g)
{
    for (int i = 0; i < 16; i++) g->rlim_cur[i] = g->rlim_max[i] = ~0ull;
    g->rlim_cur[3] = 8ull << 20;                         /* RLIMIT_STACK */
    g->rlim_max[3] = ~0ull;
    g->rlim_cur[7] = 1024;                               /* RLIMIT_NOFILE */
    g->rlim_max[7] = 65536;
    g->rlim_cur[4] = 0;                                  /* RLIMIT_CORE */
}

static struct sv_group *group_new(void)
{
    struct sv_group *g = kzalloc(sizeof(*g));
    if (!g) return NULL;
    process_t *p = kzalloc(sizeof(*p));
    if (!p) { kfree(g); return NULL; }
    g->proc = p;
    p->sv = g;
    g->refs = 1;                                         /* the share released when a parent has waited for it */
    g->umask = 022;
    g->start_ms = uptime_ms();
    rlimits_default(g);
    return g;
}

static struct sv_task *task_new(struct sv_group *g)
{
    struct sv_task *t = kzalloc(sizeof(*t));
    if (!t) return NULL;
    t->g = g;
    t->syscall_nr = -1;
    return t;
}

static void link_group(struct sv_group *g)
{
    spin_lock(&sv_lock);
    g->tgid = next_id++;
    g->next_all = sv_groups;
    sv_groups = g;
    spin_unlock(&sv_lock);
}

static int task_entry(void *arg);

/* give a task its id and put it in its group (it does not run yet) */
static void link_task(struct sv_task *t)
{
    struct sv_group *g = t->g;
    spin_lock(&sv_lock);
    t->tid = g->nthreads == 0 && !g->tasks ? g->tgid : next_id++;
    t->next = g->tasks;
    g->tasks = t;
    g->nthreads++;
    spin_unlock(&sv_lock);
    __atomic_add_fetch(&g->refs, 1, __ATOMIC_ACQ_REL);                      /* the task holds its group */
}

/* start the kernel thread of a linked task; t->fork_regs must be set */
static int launch_task(struct sv_task *t, const char *name)
{
    t->thr = thread_create_ex(name, task_entry, t, 0, -1, t->g->proc, 64 * 1024);
    if (!t->thr) return -ERR_NOMEM;
    return 0;
}

static int start_thread(struct sv_task *t, const char *name)
{
    link_task(t);
    return launch_task(t, name);
}

/* the first code of a task: enter user mode with the registers prepared for it */
static int task_entry(void *arg)
{
    struct sv_task *t = arg;
    thread_t *me = thread_current();
    me->task = t;
    me->fs_base = t->child_tls;
    wrmsr(MSR_FS_BASE, me->fs_base);
    /* the register frame is built at the top of this kernel stack, as an interrupt would have left it */
    uint8_t *top = me->kstack + me->kstack_size;
    struct regs *r = (struct regs *)(top - sizeof(struct regs) - 64);
    r = (struct regs *)ALIGN_DOWN((uint64_t)r, 16);
    *r = t->fork_regs;
    enter_user_regs(r, t->fork_fx);
}

/* a fresh x87/SSE state */
static void init_fx(void *fx)
{
    memset(fx, 0, 512);
    *(uint16_t *)fx = 0x37F;
    *(uint32_t *)((uint8_t *)fx + 24) = 0x1F80;
}

/* Fill in `regs` for the start of a program */
static void start_regs(struct regs *r, uint64_t entry, uint64_t sp)
{
    memset(r, 0, sizeof(*r));
    r->rip = entry;
    r->rsp = sp;
    r->cs = GDT_UCODE;
    r->ss = GDT_UDATA;
    r->rflags = 0x202;
}

static struct sfile *tty_file(struct tty *tty, bool write_end);

/* output of a program that has no terminal: lines of the system log */
static long log_write(struct sfile *f, const void *buf, size_t n, uint64_t *off)
{
    UNUSED(off);
    struct sv_group *g = f->priv;
    const char *s = buf;
    char line[160];
    size_t l = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\n' || l == sizeof(line) - 1) {
            line[l] = 0;
            if (l) klog("[%s] %s", g ? g->comm : "?", line);
            l = 0;
            if (s[i] == '\n') continue;
        }
        if (s[i] != '\r') line[l++] = s[i];
    }
    if (l) { line[l] = 0; klog("[%s] %s", g ? g->comm : "?", line); }
    return (long)n;
}

static uint32_t log_poll(struct sfile *f, uint32_t ev) { UNUSED(f); return ev & 4; }
static const struct sfile_ops log_ops = { .write = log_write, .poll = log_poll, .name = "log" };

static int basename_comm(const char *path, char *out, size_t n)
{
    strlcpy(out, vfs_basename(path), n);
    return 0;
}

/* Start a program from the kernel (the shell, the launcher): its parent is nobody, it ends when it ends. */
struct sv_group *sv_spawn(const char *path, char **argv, char **envp, struct tty *tty, const char *cwd)
{
    struct sv_group *g = group_new();
    if (!g) return NULL;
    struct mm *mm = mm_new();
    struct sv_elf_info info;
    char err[160];
    int argc = 0, envc = 0;
    while (argv && argv[argc]) argc++;
    while (envp && envp[envc]) envc++;
    if (!mm || sv_load_elf(mm, NULL, path, &info, err, sizeof(err))) {
        klog("abi: cannot start %s: %s", path, mm ? err : "out of memory");
        if (mm) mm_put(mm);
        kfree(g->proc);
        kfree(g);
        return NULL;
    }
    g->uid = g->euid = session.active ? (uint32_t)session.uid : 1000;
    g->gid = g->egid = g->uid;
    uint64_t a0, a1, e0, e1;
    uint64_t sp = sv_build_stack(mm, &info, argv, argc, envp, envc, path, g->uid, g->gid, &a0, &a1, &e0, &e1, err, sizeof(err));
    if (!sp) { klog("abi: cannot start %s: %s", path, err); mm_put(mm); kfree(g->proc); kfree(g); return NULL; }
    mm->brk_start = mm->brk = ALIGN_UP(info.top, PAGE_SIZE);

    g->mm = mm;
    g->arg_start = a0; g->arg_end = a1; g->env_start = e0; g->env_end = e1;
    g->files = sv_files_new();
    g->sig = sighand_new();
    g->tty = tty;
    g->pgid = g->sid = 0;
    strlcpy(g->cwd, cwd && *cwd ? cwd : "/", sizeof(g->cwd));
    strlcpy(g->exe, path, sizeof(g->exe));
    basename_comm(path, g->comm, sizeof(g->comm));
    process_t *p = g->proc;
    p->mm = mm;
    p->as = mm->as;
    p->tty = tty;
    strlcpy(p->name, g->comm, sizeof(p->name));
    strlcpy(p->cwd, g->cwd, sizeof(p->cwd));
    if (tty) tty_get(tty);
    for (int fd = 0; fd < 3; fd++) {
        struct sfile *f = tty ? tty_file(tty, fd != 0) : NULL;
        if (!f) f = sfile_new(fd == 0 ? &null_ops : &log_ops, SF_RDWR);       /* no terminal: input is empty, output goes to the system log */
        if (f && !tty && fd) f->priv = g;
        sv_fd_alloc(g, f, fd, false);
    }
    __atomic_add_fetch(&g->refs, 1, __ATOMIC_ACQ_REL);                       /* released by sv_group_put in the spawner */
    link_group(g);
    g->pgid = g->sid = g->tgid;
    p->pid = g->tgid;
    proc_register(p);

    struct sv_task *t = task_new(g);
    start_regs(&t->fork_regs, info.entry, sp);
    init_fx(t->fork_fx);
    if (start_thread(t, g->comm)) { klog("abi: no thread for %s", path); return NULL; }
    klog("abi: started '%s' (pid %d, %s)", g->comm, g->tgid, info.has_interp ? info.interp : "static");
    return g;
}

/* ------------------------------------------------------------------------
 * clone / fork / vfork
 * ---------------------------------------------------------------------- */

long sv_clone(struct regs *r, uint64_t flags, uint64_t stack, uint64_t ptid, uint64_t ctid, uint64_t tls)
{
    struct sv_task *pt = sv_cur();
    struct sv_group *pg = pt->g;
    if (flags & CLONE_NS_MASK) return -ERR_INVAL;
    if ((flags & CLONE_THREAD) && !((flags & CLONE_VM) && (flags & CLONE_SIGHAND))) return -ERR_INVAL;
    if ((flags & CLONE_SIGHAND) && !(flags & CLONE_VM)) return -ERR_INVAL;
    bool thread = flags & CLONE_THREAD;
    int exit_sig = (int)(flags & 0xFF);

    struct sv_group *g = pg;
    struct sv_task *t;
    if (thread) {
        t = task_new(pg);
        if (!t) return -ERR_NOMEM;
    } else {
        g = group_new();
        if (!g) return -ERR_NOMEM;
        g->mm = (flags & CLONE_VM) ? mm_get(pg->mm) : mm_fork(pg->mm);
        if (!g->mm) { kfree(g->proc); kfree(g); return -ERR_NOMEM; }
        g->files = (flags & CLONE_FILES) ? (__atomic_add_fetch(&pg->files->refs, 1, __ATOMIC_ACQ_REL), pg->files) : sv_files_copy(pg->files);
        if (flags & CLONE_SIGHAND) { g->sig = pg->sig; __atomic_add_fetch(&g->sig->refs, 1, __ATOMIC_ACQ_REL); }
        else { g->sig = sighand_new(); if (g->sig) memcpy(g->sig->act, pg->sig->act, sizeof(g->sig->act)); }
        if (!g->files || !g->sig) { mm_put(g->mm); sv_files_put(g->files); sighand_put(g->sig); kfree(g->proc); kfree(g); return -ERR_NOMEM; }
        /* a forked program keeps ignoring what it ignored; handlers stay (the memory is the same) */
        g->uid = pg->uid; g->euid = pg->euid; g->gid = pg->gid; g->egid = pg->egid; g->umask = pg->umask;
        g->pgid = pg->pgid; g->sid = pg->sid;
        g->tty = pg->tty;
        if (g->tty) tty_get(g->tty);
        strlcpy(g->comm, pg->comm, sizeof(g->comm));
        strlcpy(g->exe, pg->exe, sizeof(g->exe));
        strlcpy(g->cwd, pg->cwd, sizeof(g->cwd));
        memcpy(g->rlim_cur, pg->rlim_cur, sizeof(g->rlim_cur));
        memcpy(g->rlim_max, pg->rlim_max, sizeof(g->rlim_max));
        g->arg_start = pg->arg_start; g->arg_end = pg->arg_end; g->env_start = pg->env_start; g->env_end = pg->env_end;
        g->exit_signal = exit_sig;
        g->parent = (flags & CLONE_PARENT) && pg->parent ? pg->parent : pg;
        g->ppid = g->parent->tgid;
        process_t *p = g->proc;
        p->mm = g->mm;
        p->as = g->mm->as;
        p->tty = g->tty;
        strlcpy(p->name, g->comm, sizeof(p->name));
        strlcpy(p->cwd, g->cwd, sizeof(p->cwd));
        link_group(g);
        p->pid = g->tgid;
        proc_register(p);
        t = task_new(g);
        if (!t) return -ERR_NOMEM;
        __atomic_add_fetch(&pg->nchildren, 1, __ATOMIC_ACQ_REL);
    }
    t->fork_regs = *r;
    t->fork_regs.rax = 0;
    if (stack) t->fork_regs.rsp = stack;
    memcpy(t->fork_fx, (uint8_t *)r - 512, 512);
    t->child_tls = (flags & CLONE_SETTLS) ? tls : (thread_current()->fs_base);
    t->blocked = pt->blocked;
    if (flags & CLONE_CHILD_CLEARTID) t->clear_child_tid = ctid;
    struct vfork_wait *vf = NULL;
    if (flags & CLONE_VFORK) {
        vf = kzalloc(sizeof(*vf));
        if (vf) { vf->refs = 2; t->vfork = vf; }               /* one share for the child, one for the waiting parent */
    }
    /* the ids are written before the new thread can run: it may finish before this call returns */
    link_task(t);
    long tid = t->tid;
    if ((flags & CLONE_PARENT_SETTID) && ptid) copy_to_user(ptid, &(int){ (int)tid }, 4);
    if ((flags & CLONE_CHILD_SETTID) && ctid) mm_access(g->mm, ctid, &(int){ (int)tid }, 4, true);
    int rc = launch_task(t, g->comm);
    if (rc) return rc;
    /* vfork: the parent waits until the child has exec'd or exited */
    if (vf) {
        spin_lock(&sv_lock);
        while (!vf->done && !sv_signal_pending(pt)) sched_wait(vf, &sv_lock, 100);
        spin_unlock(&sv_lock);
        if (__atomic_sub_fetch(&vf->refs, 1, __ATOMIC_ACQ_REL) == 0) kfree(vf);
    }
    return tid;
}

/* ------------------------------------------------------------------------
 * exec
 * ---------------------------------------------------------------------- */

static void vfork_release(struct sv_task *t)
{
    struct vfork_wait *vf = t->vfork;
    if (!vf) return;
    t->vfork = NULL;
    vf->done = 1;
    sched_wake(vf);
    if (__atomic_sub_fetch(&vf->refs, 1, __ATOMIC_ACQ_REL) == 0) kfree(vf);
}

static void kill_other_threads(struct sv_task *me)
{
    struct sv_group *g = me->g;
    spin_lock(&sv_lock);
    for (struct sv_task *t = g->tasks; t; t = t->next)
        if (t != me) { t->exiting = true; sv_wake_task(t); }
    spin_unlock(&sv_lock);
    for (int i = 0; i < 2000; i++) {
        spin_lock(&sv_lock);
        int n = g->nthreads;
        spin_unlock(&sv_lock);
        if (n <= 1) return;
        sched_sleep(1);
    }
}

/* Replace the running program. Works on the registers of the system call so the new program starts when it returns. */
int sv_exec_file(const char *path, char **argv, char **envp, struct regs *regs, bool fresh, char *err, size_t errn)
{
    UNUSED(fresh);
    struct sv_task *me = sv_cur();
    struct sv_group *g = me->g;
    char *shebang_argv[4] = { 0 };
    char interp_arg0[VFS_PATH_MAX];
    char **av = argv;
    char realpath[VFS_PATH_MAX];
    strlcpy(realpath, path, sizeof(realpath));
    int argc = 0, envc = 0;
    while (av && av[argc]) argc++;
    while (envp && envp[envc]) envc++;

    /* scripts: "#!interpreter [argument]" */
    char **newav = NULL;
    for (int depth = 0; depth < 4; depth++) {
        vnode_t *vn = vfs_open(realpath, false);
        if (!vn) { if (err) snprintf(err, errn, "%s: no such file", realpath); kfree(newav); return -ERR_NOENT; }
        if (vn->type != VN_FILE) { vfs_close(vn); kfree(newav); return -ERR_ACCES; }
        char head[256];
        long n = vfs_read(vn, 0, head, sizeof(head) - 1);
        vfs_close(vn);
        if (n < 2 || head[0] != '#' || head[1] != '!') break;
        head[n] = 0;
        char *nl = strchr(head, '\n');
        if (nl) *nl = 0;
        char *p = head + 2;
        while (*p == ' ' || *p == '\t') p++;
        char *interp = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        char *arg = NULL;
        if (*p) { *p++ = 0; while (*p == ' ' || *p == '\t') p++; if (*p) arg = p; }
        if (!*interp) { kfree(newav); return -ERR_NOEXEC; }
        /* argv becomes: interpreter [arg] script original-args... */
        char **nv = kzalloc(sizeof(char *) * (size_t)(argc + 4));
        int k = 0;
        strlcpy(interp_arg0, interp, sizeof(interp_arg0));
        nv[k++] = interp_arg0;
        if (arg) nv[k++] = arg;
        nv[k++] = (char *)realpath;
        for (int i = 1; i < argc; i++) nv[k++] = av[i];
        nv[k] = NULL;
        kfree(newav);
        newav = nv;
        av = nv;
        argc = k;
        char tmp[VFS_PATH_MAX];
        strlcpy(tmp, interp, sizeof(tmp));
        strlcpy(realpath, tmp, sizeof(realpath));
        (void)shebang_argv;
    }

    struct mm *nm = mm_new();
    struct sv_elf_info info;
    char e2[160] = "";
    int r = nm ? sv_load_elf(nm, NULL, realpath, &info, e2, sizeof(e2)) : -ERR_NOMEM;
    if (r) {
        if (err) strlcpy(err, e2, errn);
        if (nm) mm_put(nm);
        kfree(newav);
        return r;
    }
    uint64_t a0, a1, e0, e1;
    const char *execfn = path;
    uint64_t sp = sv_build_stack(nm, &info, av, argc, envp, envc, execfn, g->euid, g->egid, &a0, &a1, &e0, &e1, e2, sizeof(e2));
    if (!sp) {
        if (err) strlcpy(err, e2, errn);
        mm_put(nm);
        kfree(newav);
        return -ERR_2BIG;
    }
    nm->brk_start = nm->brk = ALIGN_UP(info.top, PAGE_SIZE);
    nm->rlimit_stack = g->rlim_cur[3] < (1ull << 30) ? g->rlim_cur[3] : (1ull << 30);

    /* point of no return: other threads go away, the old image is dropped */
    kill_other_threads(me);
    struct mm *old = g->mm;
    g->mm = nm;
    process_t *p = g->proc;
    p->mm = nm;
    p->as = nm->as;
    thread_current()->proc = p;
    this_cpu()->active_as = nm->as;
    vmm_switch(nm->as);
    mm_put(old);
    sv_files_exec(g->files);
    spin_lock(&sv_lock);
    for (int s = 1; s <= NSIG; s++)
        if (g->sig->act[s].handler != SIG_IGN) memset(&g->sig->act[s], 0, sizeof(g->sig->act[s]));
    spin_unlock(&sv_lock);
    me->altstack_sp = me->altstack_size = 0;
    me->altstack_flags = 2;
    me->clear_child_tid = 0;
    g->arg_start = a0; g->arg_end = a1; g->env_start = e0; g->env_end = e1;
    strlcpy(g->exe, realpath[0] ? path : realpath, sizeof(g->exe));
    basename_comm(path, g->comm, sizeof(g->comm));
    strlcpy(p->name, g->comm, sizeof(p->name));
    thread_current()->fs_base = 0;
    wrmsr(MSR_FS_BASE, 0);

    start_regs(regs, info.entry, sp);
    init_fx((uint8_t *)regs - 512);
    me->syscall_nr = -1;
    vfork_release(me);                                       /* a vfork parent can go on now */
    klog("abi: exec '%s' (pid %d, %s)", g->comm, g->tgid, info.has_interp ? info.interp : "static");
    kfree(newav);
    return 0;
}

/* ------------------------------------------------------------------------
 * exit and wait
 * ---------------------------------------------------------------------- */

static void group_free(struct sv_group *g)
{
    spin_lock(&sv_lock);
    for (struct sv_group **pp = &sv_groups; *pp; pp = &(*pp)->next_all)
        if (*pp == g) { *pp = g->next_all; break; }
    spin_unlock(&sv_lock);
    proc_unregister(g->proc);
    if (g->mm) mm_put(g->mm);
    sv_files_put(g->files);
    sighand_put(g->sig);
    if (g->tty) tty_put(g->tty);
    kfree(g->proc);
    kfree(g);
}

void sv_group_put(struct sv_group *g)
{
    if (__atomic_sub_fetch(&g->refs, 1, __ATOMIC_ACQ_REL) == 0) group_free(g);
}

/* The last thread of a process is leaving: close its files, hand its children over, tell the parent. */
static void group_zombie(struct sv_group *g)
{
    sv_files_put(g->files);
    g->files = sv_files_new();                              /* an empty table keeps later lookups safe */
    spin_lock(&sv_lock);
    for (struct sv_group *c = sv_groups; c; c = c->next_all)
        if (c->parent == g) {
            c->parent = NULL;                               /* nobody waits for them any more: they clean up after themselves */
            c->ppid = 1;
            if (c->zombie && !c->reaped) { c->reaped = true; }
        }
    struct sv_group *par = g->parent;
    bool autoreap = !par || par->zombie || par->sig->act[SIGCHLD].handler == SIG_IGN || (par->sig->act[SIGCHLD].flags & SA_NOCLDWAIT);
    g->zombie = true;
    g->proc->exited = true;
    spin_unlock(&sv_lock);
    sched_wake(g);                                          /* a kernel spawner waiting for the end */
    if (par && !par->zombie) {
        if (g->exit_signal) sv_send_signal(par, NULL, g->exit_signal);
        sched_wake(par);                                    /* wait4 */
        sv_poll_wake();
    }
    if (autoreap) {
        g->reaped = true;
        sv_group_put(g);                                    /* the share held for a waiting parent */
    }
}

void sv_task_exit_hook(thread_t *th)
{
    struct sv_task *t = th->task;
    struct sv_group *g = t->g;
    /* tell joiners (pthread_join, the parent after clone) */
    if (t->clear_child_tid) {
        int zero = 0;
        if (g->mm && mm_access(g->mm, t->clear_child_tid, &zero, 4, true) == 0) sv_futex_wake_addr(g->mm, t->clear_child_tid, 1);
    }
    vfork_release(t);
    spin_lock(&sv_lock);
    for (struct sv_task **pp = &g->tasks; *pp; pp = &(*pp)->next)
        if (*pp == t) { *pp = t->next; break; }
    g->nthreads--;
    int left = g->nthreads;
    spin_unlock(&sv_lock);
    if (!left) group_zombie(g);
}

/* called by the reaper once the thread is off its stack for good */
void sv_task_reap(thread_t *th)
{
    struct sv_task *t = th->task;
    struct sv_group *g = t->g;
    kfree(t);
    th->task = NULL;
    sv_group_put(g);
}

NORETURN void sv_exit_task(int code, bool group)
{
    struct sv_task *t = sv_cur();
    struct sv_group *g = t->g;
    t->exit_status = code;
    spin_lock(&sv_lock);
    if ((group || g->nthreads == 1) && !g->exit_set) { g->exit_code = code; g->exit_set = true; }
    if (group)
        for (struct sv_task *x = g->tasks; x; x = x->next)
            if (x != t) { x->exiting = true; sv_wake_task(x); }
    spin_unlock(&sv_lock);
    thread_exit(code);
}

long sv_wait(int pid, int *status, int options, uint64_t rusage)
{
    struct sv_task *me = sv_cur();
    struct sv_group *g = me->g;
    UNUSED(rusage);
    for (;;) {
        bool have_children = false;
        struct sv_group *found = NULL;
        spin_lock(&sv_lock);
        for (struct sv_group *c = sv_groups; c; c = c->next_all) {
            if (c->parent != g || c->reaped) continue;
            bool match = pid == -1 || (pid > 0 && c->tgid == pid) || (pid == 0 && c->pgid == g->pgid) || (pid < -1 && c->pgid == -pid);
            if (!match) continue;
            have_children = true;
            if (c->zombie) { found = c; break; }
        }
        if (found) {
            found->reaped = true;
            spin_unlock(&sv_lock);
            long id = found->tgid;
            if (status) *status = found->exit_code;
            __atomic_sub_fetch(&g->nchildren, 1, __ATOMIC_ACQ_REL);
            sv_group_put(found);
            return id;
        }
        if (!have_children) { spin_unlock(&sv_lock); return -ERR_CHILD; }
        if (options & 1) { spin_unlock(&sv_lock); return 0; }                 /* WNOHANG */
        if (sv_signal_pending(me)) { spin_unlock(&sv_lock); return -ERR_INTR; }
        sched_wait(g, &sv_lock, 0);
        spin_unlock(&sv_lock);
        if (thread_current()->interrupted) { thread_current()->interrupted = 0; return -ERR_INTR; }
    }
}

/* ------------------------------------------------------------------------
 * for the kernel: wait for a process it started
 * ---------------------------------------------------------------------- */

int sv_wait_kernel(struct sv_group *g)
{
    spin_lock(&sv_lock);
    while (!g->zombie) sched_wait(g, &sv_lock, 200);
    int code = g->exit_code;
    spin_unlock(&sv_lock);
    return code;
}

/* ------------------------------------------------------------------------
 * a tty as a file
 * ---------------------------------------------------------------------- */

struct tty_priv { struct tty *tty; };

static long tty_f_read(struct sfile *f, void *buf, size_t n, uint64_t *off)
{
    UNUSED(off);
    struct tty *t = ((struct tty_priv *)f->priv)->tty;
    char tmp[512];
    long r = tty_read(t, tmp, MIN(n, sizeof(tmp)));
    if (r > 0) memcpy(buf, tmp, (size_t)r);
    return r;
}

static int echo_mode = -1;

/* a debugging aid (abi_echo=1 in system.conf): everything programs print also goes to the serial log */
static void echo_serial(const char *s, size_t n)
{
    static char line[200];
    static size_t ll;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\n' || ll >= sizeof(line) - 1) { line[ll] = 0; klog("out: %s", line); ll = 0; if (s[i] == '\n') continue; }
        if ((unsigned char)s[i] >= 32 || s[i] == '\t') line[ll++] = s[i];
    }
}

static long tty_f_write(struct sfile *f, const void *buf, size_t n, uint64_t *off)
{
    UNUSED(off);
    struct tty *t = ((struct tty_priv *)f->priv)->tty;
    if (echo_mode < 0) echo_mode = (int)conf_int(&sysconf, "abi_echo", 0);
    if (echo_mode) echo_serial(buf, n);
    /* the terminal wants LF as CR LF */
    const char *s = buf;
    size_t start = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\n') {
            tty_write(t, s + start, i - start);
            tty_write(t, "\r\n", 2);
            start = i + 1;
        }
    }
    if (start < n) tty_write(t, s + start, n - start);
    return (long)n;
}

static long tty_f_ioctl(struct sfile *f, unsigned long cmd, uint64_t arg)
{
    struct tty *t = ((struct tty_priv *)f->priv)->tty;
    switch (cmd) {
    case 0x5413: {                                         /* TIOCGWINSZ */
        uint16_t ws[4] = { (uint16_t)(t->rows ? t->rows : 24), (uint16_t)(t->cols ? t->cols : 80), 0, 0 };
        return copy_to_user(arg, ws, sizeof(ws)) ? -ERR_FAULT : 0;
    }
    case 0x5401: {                                         /* TCGETS: a plain terminal in canonical mode */
        uint32_t tio[9];
        memset(tio, 0, sizeof(tio));
        tio[0] = 0x2D02; tio[1] = 5; tio[2] = 0xBF; tio[3] = 0x8A3B;
        return copy_to_user(arg, tio, 36) ? -ERR_FAULT : 0;
    }
    case 0x5402: case 0x5403: case 0x5404: return 0;       /* TCSETS* */
    case 0x540F: return t->fg_pid;                         /* TIOCGPGRP */
    case 0x5410: return 0;                                 /* TIOCSPGRP */
    default: return -ERR_NOTTY;
    }
}

static uint32_t tty_f_poll(struct sfile *f, uint32_t ev)
{
    struct tty *t = ((struct tty_priv *)f->priv)->tty;
    uint32_t r = ev & 4;                                   /* POLLOUT */
    spin_lock(&t->lock);
    if (t->rhead != t->rtail || t->eof || t->closed) r |= ev & 1;
    spin_unlock(&t->lock);
    return r;
}

static void tty_f_close(struct sfile *f)
{
    struct tty_priv *p = f->priv;
    tty_put(p->tty);
    kfree(p);
}

const struct sfile_ops tty_ops = {
    .read = tty_f_read, .write = tty_f_write, .ioctl = tty_f_ioctl, .poll = tty_f_poll, .close = tty_f_close, .name = "tty",
};

static struct sfile *tty_file(struct tty *tty, bool write_end)
{
    UNUSED(write_end);
    struct sfile *f = sfile_new(&tty_ops, SF_RDWR);
    if (!f) return NULL;
    struct tty_priv *p = kzalloc(sizeof(*p));
    p->tty = tty;
    tty_get(tty);
    f->priv = p;
    return f;
}

/* a text dump of every System V task (the shell's `svps`) */
int sv_dump(char *out, size_t n)
{
    size_t o = 0;
    spin_lock(&sv_lock);
    for (struct sv_group *g = sv_groups; g && o + 200 < n; g = g->next_all) {
        o += (size_t)snprintf(out + o, n - o, "pid %d %s %s ppid %d threads %d refs %d rss %lu pages\n", g->tgid, g->comm, g->zombie ? "ZOMBIE" : "",
                              g->parent ? g->parent->tgid : g->ppid, g->nthreads, g->refs, g->mm ? g->mm->rss_pages : 0);
        for (struct sv_task *t = g->tasks; t && o + 160 < n; t = t->next) {
            const char *st = !t->thr ? "?" : t->thr->state == T_BLOCKED ? "sleep" : t->thr->state == T_RUNNING ? "run" : "ready";
            o += (size_t)snprintf(out + o, n - o, "  tid %d %s call %d wait %p blk %lx pnd %lx grp %lx exiting %d\n", t->tid, st, t->syscall_nr,
                                  t->thr ? t->thr->wait_chan : NULL, t->blocked, t->pending, g->pending, t->exiting);
        }
    }
    spin_unlock(&sv_lock);
    return (int)o;
}

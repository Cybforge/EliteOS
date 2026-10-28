/*
 * User processes: ELF loading into a private address space, ring-3 entry,
 * file descriptors, exit/wait/kill, fault isolation.
 */
#include <kernel.h>
#include <cpu.h>
#include <sched.h>
#include <dev.h>
#include <vfs.h>
#include <tty.h>
#include <wm.h>
#include <x86.h>
#include "file.h"

#define MAX_PROCS 512

static process_t *procs[MAX_PROCS];
static spinlock_t proc_lock = SPINLOCK_INIT("procs");
static int next_pid = 100;

struct proc_extra {
    int refs;
    bool detached;
    uint64_t entry;
    uint64_t user_sp;
    int argc;
    uint64_t argv;
};

/* the extra bookkeeping lives right after the process struct */
struct proc_full {
    process_t p;
    struct proc_extra x;
};

#define X(p) (&((struct proc_full *)(p))->x)

/* ------------------------------------------------------------------------
 * ELF
 * ---------------------------------------------------------------------- */

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    uint32_t p_type, p_flags;
    uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
} Elf64_Phdr;

static bool map_user_page(process_t *p, uint64_t va, uint64_t flags)
{
    if (vmm_translate(p->as, va)) return true;
    uint64_t pa = pmm_alloc();
    if (!pa) return false;
    if (!vmm_map(p->as, va, pa, flags | PTE_U | PTE_OWNED)) { pmm_free(pa); return false; }
    p->mem_pages++;
    return true;
}

static bool load_elf(process_t *p, const uint8_t *img, size_t size)
{
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)img;
    if (size < sizeof(*eh) || memcmp(eh->e_ident, "\x7F" "ELF", 4) || eh->e_ident[4] != 2 || eh->e_machine != 0x3E)
        return false;
    uint64_t top = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr *ph = (const Elf64_Phdr *)(img + eh->e_phoff + (uint64_t)i * eh->e_phentsize);
        if (ph->p_type != 1) continue;
        if (ph->p_vaddr < USER_BASE || ph->p_vaddr + ph->p_memsz > USER_STACK_TOP - USER_STACK_SIZE) return false;
        if (ph->p_offset + ph->p_filesz > size) return false;
        uint64_t flags = 0;
        if (ph->p_flags & 2) flags |= PTE_W;
        if (!(ph->p_flags & 1) && cpu_info.nx) flags |= PTE_NX;
        for (uint64_t va = ALIGN_DOWN(ph->p_vaddr, PAGE_SIZE); va < ph->p_vaddr + ph->p_memsz; va += PAGE_SIZE) {
            if (!map_user_page(p, va, flags | PTE_W)) return false;
        }
        /* copy through the direct map, page by page */
        for (uint64_t off = 0; off < ph->p_filesz;) {
            uint64_t va = ph->p_vaddr + off;
            uint64_t pa = vmm_translate(p->as, va);
            uint64_t n = MIN(ph->p_filesz - off, PAGE_SIZE - (va & 0xFFF));
            memcpy(P2V(pa), img + ph->p_offset + off, n);
            off += n;
        }
        /* drop write permission on read-only segments now that they are filled */
        if (!(ph->p_flags & 2))
            for (uint64_t va = ALIGN_DOWN(ph->p_vaddr, PAGE_SIZE); va < ph->p_vaddr + ph->p_memsz; va += PAGE_SIZE) {
                uint64_t pa = vmm_translate(p->as, va);
                vmm_map(p->as, va, pa, flags | PTE_U | PTE_OWNED);
            }
        top = MAX(top, ph->p_vaddr + ph->p_memsz);
    }
    p->brk_start = p->brk = ALIGN_UP(top, PAGE_SIZE);
    X(p)->entry = eh->e_entry;
    return true;
}

static bool setup_stack(process_t *p, int argc, char **argv)
{
    for (uint64_t va = USER_STACK_TOP - USER_STACK_SIZE; va < USER_STACK_TOP; va += PAGE_SIZE)
        if (!map_user_page(p, va, PTE_W | (cpu_info.nx ? PTE_NX : 0))) return false;
    /* strings at the very top, then the pointer array */
    uint64_t sp = USER_STACK_TOP;
    uint64_t ptrs[32];
    argc = MIN(argc, 31);
    for (int i = argc - 1; i >= 0; i--) {
        size_t n = strlen(argv[i]) + 1;
        sp -= n;
        uint64_t pa = vmm_translate(p->as, sp);
        /* strings may straddle a page: copy byte-wise through translate */
        for (size_t k = 0; k < n; k++) {
            pa = vmm_translate(p->as, sp + k);
            *(char *)P2V(pa) = argv[i][k];
        }
        ptrs[i] = sp;
    }
    ptrs[argc] = 0;
    sp = ALIGN_DOWN(sp, 16);
    sp -= (uint64_t)(argc + 1) * 8;
    sp = ALIGN_DOWN(sp, 16);
    for (int i = 0; i <= argc; i++) {
        uint64_t pa = vmm_translate(p->as, sp + (uint64_t)i * 8);
        *(uint64_t *)P2V(pa) = ptrs[i];
    }
    X(p)->argv = sp;
    X(p)->argc = argc;
    X(p)->user_sp = sp - 8;        /* as if a return address had been pushed */
    return true;
}

/* ------------------------------------------------------------------------
 * lifecycle
 * ---------------------------------------------------------------------- */

static int proc_entry(void *arg)
{
    process_t *p = arg;
    enter_user(X(p)->entry, X(p)->user_sp, (uint64_t)X(p)->argc, X(p)->argv);
}

static void proc_free(process_t *p)
{
    spin_lock(&proc_lock);
    for (int i = 0; i < MAX_PROCS; i++)
        if (procs[i] == p) procs[i] = NULL;
    spin_unlock(&proc_lock);
    kfree(p);
}

static void put_ref(process_t *p)
{
    if (__atomic_sub_fetch(&X(p)->refs, 1, __ATOMIC_ACQ_REL) == 0) proc_free(p);
}

process_t *proc_spawn(const char *path, int argc, char **argv, struct tty *tty, const char *cwd)
{
    size_t size;
    uint8_t *img = (uint8_t *)vfs_read_file(path, &size);
    if (!img) return NULL;
    struct proc_full *pf = kzalloc(sizeof(*pf));
    process_t *p = &pf->p;
    p->as = vmm_create_space();
    if (!p->as || !load_elf(p, img, size) || !setup_stack(p, argc, argv)) {
        kfree(img);
        if (p->as) vmm_destroy_space(p->as);
        kfree(pf);
        return NULL;
    }
    kfree(img);
    strlcpy(p->name, vfs_basename(path), sizeof(p->name));
    strlcpy(p->cwd, cwd ? cwd : "/", sizeof(p->cwd));
    p->tty = tty;
    X(p)->detached = tty == NULL;
    X(p)->refs = X(p)->detached ? 1 : 2;

    /* stdin/stdout/stderr */
    for (int fd = 0; fd < 3; fd++) {
        struct file *f = kzalloc(sizeof(*f));
        f->refs = 1;
        if (tty) { f->kind = F_TTY; f->tty = tty; tty_get(tty); }
        else f->kind = fd == 0 ? F_NULL : F_LOG;
        p->fds[fd] = f;
    }

    spin_lock(&proc_lock);
    p->pid = next_pid++;
    for (int i = 0; i < MAX_PROCS; i++)
        if (!procs[i]) { procs[i] = p; break; }
    spin_unlock(&proc_lock);

    thread_t *t = thread_create_ex(p->name, proc_entry, p, 0, -1, p, 32 * 1024);
    p->main = t;
    klog("proc: started '%s' (pid %d, %lu KiB)", p->name, p->pid, (p->mem_pages * PAGE_SIZE) >> 10);
    return p;
}

/* processes of the System V interface appear in the same list (system monitor, kill) */
void proc_register(process_t *p)
{
    spin_lock(&proc_lock);
    for (int i = 0; i < MAX_PROCS; i++)
        if (!procs[i]) { procs[i] = p; break; }
    spin_unlock(&proc_lock);
}

void proc_unregister(process_t *p)
{
    spin_lock(&proc_lock);
    for (int i = 0; i < MAX_PROCS; i++)
        if (procs[i] == p) procs[i] = NULL;
    spin_unlock(&proc_lock);
}

process_t *proc_find(int pid)
{
    process_t *r = NULL;
    spin_lock(&proc_lock);
    for (int i = 0; i < MAX_PROCS; i++)
        if (procs[i] && procs[i]->pid == pid) r = procs[i];
    spin_unlock(&proc_lock);
    return r;
}

int proc_list(int *pids, int max)
{
    int n = 0;
    spin_lock(&proc_lock);
    for (int i = 0; i < MAX_PROCS && n < max; i++)
        if (procs[i] && !procs[i]->exited) pids[n++] = procs[i]->pid;
    spin_unlock(&proc_lock);
    return n;
}

int proc_wait(int pid)
{
    process_t *p = proc_find(pid);
    if (!p || X(p)->detached) return -1;
    static spinlock_t wl = SPINLOCK_INIT("proc-wait");
    spin_lock(&wl);
    while (!p->exited) sched_wait(p, &wl, 200);
    spin_unlock(&wl);
    int code = p->exit_code;
    put_ref(p);
    return code;
}

void sv_send_signal(struct sv_group *g, struct sv_task *t, int sig);

bool proc_kill(int pid)
{
    process_t *p = proc_find(pid);
    if (!p || p->exited) return false;
    if (p->sv) { sv_send_signal(p->sv, NULL, 9); return true; }
    p->killed = true;
    return true;
}

/* Called on the dying process's own thread (from thread_exit). */
void proc_exit(int code)
{
    thread_t *t = thread_current();
    process_t *p = t->proc;
    for (int i = 0; i < p->nwindows; i++)
        if (p->windows[i]) { wm_destroy(p->windows[i]); p->windows[i] = NULL; }
    for (int fd = 0; fd < MAX_FDS; fd++)
        if (p->fds[fd]) { file_put(p->fds[fd]); p->fds[fd] = NULL; }
    p->exit_code = code;
    klog("proc: '%s' (pid %d) exited with %d", p->name, p->pid, code);
    __atomic_store_n(&p->exited, true, __ATOMIC_RELEASE);
    sched_wake(p);
    thread_exit(code);
}

/* Reaper context: the thread is off-CPU for good, free the address space. */
void proc_reap(thread_t *t)
{
    process_t *p = t->proc;
    vmm_destroy_space(p->as);
    p->as = NULL;
    put_ref(p);
}

void sv_fault(struct regs *r);
void sv_return_to_user(struct regs *r);

void proc_fault(struct regs *r)
{
    thread_t *t = thread_current();
    process_t *p = t->proc;
    if (p && p->sv) { sv_fault(r); return; }
    const char *exception_name(int v);
    char msg[160];
    snprintf(msg, sizeof(msg), "\n\x1b[91m%s: %s at %#lx", p ? p->name : "?", exception_name((int)r->vector), r->rip);
    if (r->vector == 14) {
        char more[48];
        snprintf(more, sizeof(more), " (address %#lx)", read_cr2());
        strlcat(msg, more, sizeof(msg));
    }
    strlcat(msg, " - process terminated\x1b[0m\n", sizeof(msg));
    klog("proc: %s", msg + 6);
    if (p && p->tty) tty_write(p->tty, msg, strlen(msg));
    else if (p) wm_notify("Program crashed", msg + 6, ICON_APP);
    sti();
    proc_exit(-1000 - (int)r->vector);
}

/* Checked when returning to user mode (syscalls and interrupts). */
void proc_check_killed(struct regs *r)
{
    thread_t *t = thread_current();
    if (t->proc && t->proc->sv) { sv_return_to_user(r); return; }
    if (t->proc && t->proc->killed && !t->proc->exited) {
        sti();
        proc_exit(-9);
    }
}

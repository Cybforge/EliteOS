#ifndef ZENITH_SCHED_H
#define ZENITH_SCHED_H

#include <kernel.h>
#include <spinlock.h>
#include <mm.h>

enum thread_state {
    T_READY,
    T_RUNNING,
    T_BLOCKED,
    T_DEAD,
};

struct process;
struct regs;
struct mm;
struct sv_group;
struct sv_task;

typedef struct thread {
    uint64_t rsp;               /* saved kernel stack pointer */
    uint8_t *kstack;
    size_t kstack_size;
    int tid;
    char name[32];
    volatile int state;
    int prio;                   /* 0 normal, 1 interactive (checked first) */
    int affinity;               /* -1 any, else cpu id */
    int last_cpu;
    struct process *proc;
    struct thread *next;        /* ready queue */
    struct thread *all_next;    /* list of all threads */
    struct thread *blk_next;    /* blocked list */
    void *wait_chan;
    uint64_t wake_at;           /* ms, 0 = no timeout */
    int timed_out;
    uint64_t ticks;             /* cpu time in ms */
    uint64_t ticks_mark;
    uint32_t cpu_pct;           /* x10, last second */
    uint64_t created_at;
    int exit_code;
    uint64_t fs_base;           /* user TLS pointer of System V threads */
    uint64_t fault_addr;        /* CR2 of the page fault being handled (read before interrupts come back on) */
    struct sv_task *task;       /* ... and their per-thread state */
    volatile int interrupted;   /* woken by a signal while waiting */
} thread_t;

struct thread_snapshot {
    int tid;
    int pid;
    char name[32];
    int state;
    int cpu;
    uint32_t cpu_pct;   /* x10 */
    uint64_t ticks;
    uint64_t mem;       /* bytes (user processes) */
    bool user;
};

void sched_init(void);
void sched_start_ap(void);                 /* per-AP idle loop, never returns */
thread_t *thread_create(const char *name, int (*fn)(void *), void *arg);
thread_t *thread_create_ex(const char *name, int (*fn)(void *), void *arg, int prio, int affinity,
                           struct process *proc, size_t stack_size);
NORETURN void thread_exit(int code);
thread_t *thread_current(void);
void sched_yield(void);
void sched_sleep(uint64_t ms);
int  sched_wait(void *chan, spinlock_t *lk, uint64_t timeout_ms);  /* 1 = timed out */
void sched_wake(void *chan);
void sched_tick(void);
void sched_preempt(void);
void sched_thread_started(void);
int  sched_snapshot(struct thread_snapshot *out, int max);
bool sched_kill_tid(int tid);
int  sched_thread_count(void);
uint64_t sched_context_switches(void);

/* process.c */
#define MAX_FDS 32

struct file;
struct tty;

typedef struct process {
    int pid;
    char name[32];
    addrspace_t *as;
    thread_t *main;
    struct file *fds[MAX_FDS];
    uint64_t brk_start, brk;
    uint64_t mem_pages;
    int exit_code;
    volatile bool exited;
    volatile bool killed;
    struct tty *tty;
    char cwd[128];
    int parent;
    struct process *next;
    int nwindows;
    void *windows[8];
    struct mm *mm;              /* programs of the System V interface: their memory (as == mm->as) */
    struct sv_group *sv;        /* ... and their process state; NULL for ZenithOS programs */
} process_t;

process_t *proc_spawn(const char *path, int argc, char **argv, struct tty *tty, const char *cwd);
process_t *proc_find(int pid);
int  proc_wait(int pid);
void proc_exit(int code);
void proc_fault(struct regs *r);
bool proc_kill(int pid);
void proc_reap(thread_t *t);
int  proc_list(int *pids, int max);

#endif

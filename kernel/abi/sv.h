#ifndef ZENITH_SV_H
#define ZENITH_SV_H

/*
 * Processes and threads of programs that use the System V x86-64 program
 * interface (the system call numbers, structure layouts and process model that
 * existing x86-64 ELF programs expect). Written from the interface's public
 * description; nothing here is taken from another operating system.
 */
#include <kernel.h>
#include <cpu.h>
#include <sched.h>
#include <vfs.h>
#include <umm.h>
#include <abierr.h>

typedef uint64_t sigset64_t;
#define SIGBIT(s) (1ull << ((s) - 1))

enum {
    SIGHUP = 1, SIGINT, SIGQUIT, SIGILL, SIGTRAP, SIGABRT, SIGBUS, SIGFPE, SIGKILL, SIGUSR1, SIGSEGV, SIGUSR2, SIGPIPE,
    SIGALRM, SIGTERM, SIGSTKFLT, SIGCHLD, SIGCONT, SIGSTOP, SIGTSTP, SIGTTIN, SIGTTOU, SIGURG, SIGXCPU, SIGXFSZ,
    SIGVTALRM, SIGPROF, SIGWINCH, SIGIO, SIGPWR, SIGSYS, SIGRTMIN = 32, NSIG = 64
};

#define SA_NOCLDSTOP 1u
#define SA_NOCLDWAIT 2u
#define SA_SIGINFO   4u
#define SA_RESTORER  0x04000000u
#define SA_ONSTACK   0x08000000u
#define SA_RESTART   0x10000000u
#define SA_NODEFER   0x40000000u
#define SA_RESETHAND 0x80000000u

#define SIG_DFL 0ull
#define SIG_IGN 1ull

struct sv_sigaction {
    uint64_t handler;
    uint64_t flags;
    uint64_t restorer;
    sigset64_t mask;
};

struct sv_sighand {
    int refs;
    struct sv_sigaction act[NSIG + 1];
};

/* ---- open files ---- */

struct sfile;

struct sfile_ops {
    long (*read)(struct sfile *f, void *buf, size_t n, uint64_t *off);          /* kernel buffer; *off is the position to use */
    long (*write)(struct sfile *f, const void *buf, size_t n, uint64_t *off);
    long (*ioctl)(struct sfile *f, unsigned long cmd, uint64_t arg);
    uint32_t (*poll)(struct sfile *f, uint32_t events);                         /* ready events */
    long (*mmap)(struct sfile *f, struct mm *m, uint64_t addr, uint64_t len, uint32_t prot, uint32_t flags, bool fixed, uint64_t off);
    int  (*stat)(struct sfile *f, struct vfs_stat *st, uint32_t *mode, uint64_t *ino);
    long (*getdents)(struct sfile *f, uint8_t *buf, size_t n);
    long (*seek)(struct sfile *f, int64_t off, int whence);
    long (*truncate)(struct sfile *f, uint64_t size);
    void (*close)(struct sfile *f);
    const char *name;
};

#define SF_NONBLOCK 0x800
#define SF_APPEND   0x400
#define SF_CLOEXEC  0x80000
#define SF_RDONLY 0
#define SF_WRONLY 1
#define SF_RDWR   2

struct sfile {
    int refs;
    int flags;                       /* O_* of the open file description */
    uint64_t off;
    mutex_t lock;
    const struct sfile_ops *ops;
    void *priv;
    vnode_t *vn;
    char path[VFS_PATH_MAX];         /* for /proc/<pid>/fd and fchdir */
    uint32_t dirpos;
};

struct sfile *sfile_new(const struct sfile_ops *ops, int flags);
struct sfile *sfile_get(struct sfile *f);
void          sfile_put(struct sfile *f);

struct sv_files {
    int refs;
    mutex_t lock;
    struct sfile **fd;
    uint8_t *cloexec;
    int cap;
};

/* ---- processes and threads ---- */

struct sv_task {
    int tid;
    struct sv_group *g;
    thread_t *thr;
    struct sv_task *next;            /* the group's threads */
    sigset64_t blocked, pending;
    sigset64_t saved_mask;           /* restored after a signal handler ran from sigsuspend/ppoll */
    bool has_saved_mask;
    uint64_t altstack_sp, altstack_size;
    uint32_t altstack_flags;
    uint64_t clear_child_tid;
    uint64_t robust_list;
    uint64_t sigsegv_addr;           /* address of the fault being reported */
    int sigsegv_code;
    int syscall_nr;                  /* the system call in progress (for restarting) */
    volatile bool exiting;
    volatile bool in_futex;
    struct vfork_wait *vfork;        /* set while the parent of a vfork child is suspended */
    int exit_status;
    void *wait_obj;
    int tgkill_sig;
    struct regs fork_regs;           /* a new child starts from these */
    uint8_t fork_fx[512] __attribute__((aligned(16)));
    uint64_t child_stack;            /* clone: the child's stack pointer */
    uint64_t child_tls;
    int clone_flags;
    uint64_t child_tid_user;
    uint64_t exit_futex;
};

struct vfork_wait {
    int refs;
    volatile int done;
};

struct sv_group {
    int tgid;
    int refs;                        /* tasks + the share held for a waiting parent + the kernel spawner */
    bool exit_set;
    process_t *proc;
    struct mm *mm;
    struct sv_files *files;
    struct sv_sighand *sig;
    struct sv_task *tasks;
    int nthreads;
    char comm[16];
    char exe[VFS_PATH_MAX];
    char cwd[VFS_PATH_MAX];
    struct sv_group *parent;
    int ppid;
    int pgid, sid;
    uint32_t uid, gid, euid, egid;
    uint32_t umask;
    sigset64_t pending;              /* directed at the process */
    int exit_code;                   /* wait status */
    volatile bool zombie;
    volatile bool reaped;
    int exit_signal;                 /* signal sent to the parent at exit */
    uint64_t start_ms;
    uint64_t utime_ms, stime_ms;
    struct tty *tty;
    uint64_t rlim_cur[16], rlim_max[16];
    struct sv_group *next_all;
    int nchildren;
    int fork_count;
    uint64_t alarm_at, itimer_interval;
    char **saved_argv;
    int nargs;
    uint64_t arg_start, arg_end, env_start, env_end;
    uint64_t auxv_addr;
    bool stopped;
};

extern struct sv_group *sv_groups;

struct sv_task *sv_cur(void);
struct sv_group *sv_curgrp(void);

/* task.c */
int  sv_exec_file(const char *path, char **argv, char **envp, struct regs *regs, bool fresh, char *err, size_t errn);
struct sv_group *sv_spawn(const char *path, char **argv, char **envp, struct tty *tty, const char *cwd);
long sv_clone(struct regs *r, uint64_t flags, uint64_t stack, uint64_t ptid, uint64_t ctid, uint64_t tls);
NORETURN void sv_exit_task(int code, bool group);
long sv_wait(int pid, int *status, int options, uint64_t rusage);
void sv_return_to_user(struct regs *r);
void sv_fault(struct regs *r);
bool sv_signal_pending(struct sv_task *t);
int  sv_kill_group(int tgid, int sig);
int  sv_kill_task(int tgid, int tid, int sig);
void sv_send_signal(struct sv_group *g, struct sv_task *t, int sig);
void sv_wake_task(struct sv_task *t);
long sv_rt_sigreturn(struct regs *r);
long sv_sigaction(int sig, uint64_t act, uint64_t old, size_t size);
long sv_sigprocmask(int how, uint64_t set, uint64_t old, size_t size);
long sv_sigaltstack(uint64_t ss, uint64_t old);
long sv_sigsuspend(uint64_t mask, size_t size);
long sv_sigpending(uint64_t set, size_t size);
struct sv_group *sv_find_group(int tgid);
struct sv_task *sv_find_task(int tid);
int  sv_group_list(int *ids, int max);
void sv_tick(void);
void sv_group_put(struct sv_group *g);
void sv_task_exit_hook(thread_t *th);
void sv_task_reap(thread_t *th);
int  sv_wait_kernel(struct sv_group *g);

/* files */
struct sv_files *sv_files_new(void);
struct sv_files *sv_files_copy(struct sv_files *f);
void sv_files_put(struct sv_files *f);
int  sv_fd_alloc(struct sv_group *g, struct sfile *f, int minfd, bool cloexec);
struct sfile *sv_fd_get(struct sv_group *g, int fd);          /* takes a reference */
int  sv_fd_close(struct sv_group *g, int fd);
void sv_files_exec(struct sv_files *f);                       /* closes the close-on-exec descriptors */

/* the system call layer */
void sv_syscall(struct regs *r);
bool sv_abi_ready(void);
int  sv_open_path(struct sv_group *g, const char *path, int flags, int mode, struct sfile **out);
extern const struct sfile_ops vnode_ops, dir_ops, null_ops, tty_ops, zero_ops;
void sv_poll_wake(void);                                       /* something became readable/writable somewhere */
long sv_poll_wait(uint64_t timeout_ms);                        /* sleep until sv_poll_wake or timeout; -ERR_INTR if a signal came */
int  sv_proc_path(struct sv_group *g, const char *path, struct sfile **out);   /* /proc, /dev, /sys style files */

/* futex.c */
long sv_futex(uint64_t uaddr, int op, uint32_t val, uint64_t timeout, uint64_t uaddr2, uint32_t val3);
void sv_futex_wake_addr(struct mm *m, uint64_t uaddr, int count);

/* time helpers */
uint64_t sv_now_ns(void);                                     /* monotonic */
uint64_t sv_realtime_ns(void);

/* the program loader */
struct sv_elf_info {
    uint64_t entry;              /* where execution starts (the interpreter's, if there is one) */
    uint64_t phdr, phnum, phent;
    uint64_t base;               /* load bias of the main program */
    uint64_t interp_base;
    uint64_t top;                /* end of the program image, for brk */
    uint64_t prog_entry;
    char interp[VFS_PATH_MAX];
    bool has_interp;
};
int sv_load_elf(struct mm *m, struct sfile *f, const char *path, struct sv_elf_info *out, char *err, size_t errn);
uint64_t sv_build_stack(struct mm *m, const struct sv_elf_info *info, char **argv, int argc, char **envp, int envc,
                        const char *execfn, uint32_t uid, uint32_t gid, uint64_t *arg_start, uint64_t *arg_end,
                        uint64_t *env_start, uint64_t *env_end, char *err, size_t errn);

#endif

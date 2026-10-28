#ifndef ZENITH_SVCALLS_H
#define ZENITH_SVCALLS_H

/* the system call functions that live outside sysv.c; each takes the saved registers (arguments in rdi rsi rdx r10 r8 r9) */
#include <cpu.h>
#include "sv.h"

#define SC(name) long name(struct regs *r)

/* fsys.c */
SC(s_read); SC(s_write); SC(s_open); SC(s_close); SC(s_stat); SC(s_fstat); SC(s_lstat); SC(s_lseek); SC(s_ioctl);
SC(s_pread64); SC(s_pwrite64); SC(s_readv); SC(s_writev); SC(s_access); SC(s_dup); SC(s_dup2); SC(s_dup3); SC(s_fcntl);
SC(s_truncate); SC(s_ftruncate); SC(s_getdents); SC(s_getdents64); SC(s_getcwd); SC(s_chdir); SC(s_fchdir);
SC(s_rename); SC(s_mkdir); SC(s_rmdir); SC(s_creat); SC(s_unlink); SC(s_readlink); SC(s_openat); SC(s_mkdirat);
SC(s_newfstatat); SC(s_unlinkat); SC(s_renameat); SC(s_renameat2); SC(s_readlinkat); SC(s_faccessat); SC(s_statx);
SC(s_statfs); SC(s_fstatfs); SC(s_fallocate); SC(s_sendfile); SC(s_close_range); SC(s_copy_file_range);
SC(s_preadv); SC(s_pwritev); SC(s_memfd_create); SC(s_enodata); SC(s_pipe); SC(s_pipe2);
/* poll.c */
SC(s_poll); SC(s_ppoll); SC(s_select); SC(s_pselect6); SC(s_epoll_create); SC(s_epoll_create1); SC(s_epoll_ctl);
SC(s_epoll_wait); SC(s_epoll_pwait); SC(s_epoll_pwait2); SC(s_eventfd); SC(s_eventfd2); SC(s_timerfd_create);
SC(s_timerfd_settime); SC(s_timerfd_gettime); SC(s_signalfd); SC(s_signalfd4); SC(s_inotify_init); SC(s_inotify_add_watch);
/* sock.c */
SC(s_socket); SC(s_connect); SC(s_accept); SC(s_accept4); SC(s_sendto); SC(s_recvfrom); SC(s_sendmsg); SC(s_recvmsg);
SC(s_shutdown); SC(s_bind); SC(s_listen); SC(s_getsockname); SC(s_getpeername); SC(s_socketpair); SC(s_setsockopt);
SC(s_getsockopt); SC(s_sendmmsg); SC(s_recvmmsg);
/* misc.c */
SC(s_futex); SC(s_capget); SC(s_alarm); SC(s_setitimer); SC(s_getitimer); SC(s_timer_create); SC(s_timer_settime);
SC(s_timer_gettime); SC(s_shmget); SC(s_shmat); SC(s_shmctl);

int  sv_wait_chan(void *chan, spinlock_t *lk, uint64_t timeout_ms);
long sv_sleep_ms(uint64_t ms);
struct sfile *sv_pipe_pair(struct sfile **write_end, int flags);
struct sfile *sv_memfd_new(const char *name, uint32_t flags);
void sv_abi_init(void);

#endif

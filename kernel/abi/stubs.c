/* system calls that are not implemented yet answer "function not implemented" */
#include <kernel.h>
#include <abierr.h>
#include "sv.h"
#include "svcalls.h"

#define STUB(n) long n(struct regs *r) { (void)r; return -ERR_NOSYS; }
STUB(s_poll) STUB(s_ppoll) STUB(s_select) STUB(s_pselect6) STUB(s_epoll_create) STUB(s_epoll_create1) STUB(s_epoll_ctl)
STUB(s_epoll_wait) STUB(s_epoll_pwait) STUB(s_epoll_pwait2) STUB(s_eventfd) STUB(s_eventfd2) STUB(s_timerfd_create)
STUB(s_timerfd_settime) STUB(s_timerfd_gettime) STUB(s_signalfd) STUB(s_signalfd4) STUB(s_inotify_init) STUB(s_inotify_add_watch)
STUB(s_socket) STUB(s_connect) STUB(s_accept) STUB(s_accept4) STUB(s_sendto) STUB(s_recvfrom) STUB(s_sendmsg) STUB(s_recvmsg)
STUB(s_shutdown) STUB(s_bind) STUB(s_listen) STUB(s_getsockname) STUB(s_getpeername) STUB(s_socketpair) STUB(s_setsockopt)
STUB(s_getsockopt) STUB(s_sendmmsg) STUB(s_recvmmsg)

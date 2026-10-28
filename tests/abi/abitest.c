#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <setjmp.h>
#include <pthread.h>
#include <time.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <sys/signalfd.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/syscall.h>
#include <sys/random.h>
#include <sys/resource.h>

static int fails;
#define CHECK(cond, name) do { if (cond) printf("ok    %s\n", name); else { printf("FAIL  %s (errno %d)\n", name, errno); fails++; } fflush(stdout); } while (0)

static volatile int got_usr1, got_alrm;
static sigjmp_buf jb;
static void on_usr1(int s) { got_usr1 = s; }
static void on_alrm(int s) { got_alrm = s; }
static void on_segv(int s) { siglongjmp(jb, 1); }

static int counter;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int ready;
static void *worker(void *a)
{
    for (int i = 0; i < 1000; i++) { pthread_mutex_lock(&mu); counter++; pthread_mutex_unlock(&mu); }
    return a;
}
static void *waiter(void *a)
{
    pthread_mutex_lock(&mu);
    while (!ready) pthread_cond_wait(&cv, &mu);
    pthread_mutex_unlock(&mu);
    return a;
}

int main(int argc, char **argv)
{
    printf("abitest start\n"); fflush(stdout);
    /* fork + wait + copy on write */
    int shared = 5;
    pid_t c = fork();
    if (c == 0) { shared = 99; _exit(7); }
    int st = 0;
    pid_t w = waitpid(c, &st, 0);
    CHECK(w == c && WIFEXITED(st) && WEXITSTATUS(st) == 7, "fork/wait exit status");
    CHECK(shared == 5, "fork copy-on-write isolation");

    /* pipe between processes */
    int p[2];
    CHECK(pipe(p) == 0, "pipe");
    c = fork();
    if (c == 0) { close(p[0]); const char *m = "through the pipe"; write(p[1], m, strlen(m)); _exit(0); }
    close(p[1]);
    char buf[64] = {0};
    ssize_t n = read(p[0], buf, sizeof(buf) - 1);
    waitpid(c, &st, 0);
    CHECK(n == 16 && !strcmp(buf, "through the pipe"), "pipe data");
    n = read(p[0], buf, sizeof(buf));
    CHECK(n == 0, "pipe EOF after writer exit");
    close(p[0]);

    /* exec */
    c = fork();
    if (c == 0) { execl("/disk/hello", "hello", "x", (char *)0); _exit(126); }
    waitpid(c, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 3, "fork+execve of another program");

    /* signals */
    struct sigaction sa; memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_usr1; sigaction(SIGUSR1, &sa, NULL);
    kill(getpid(), SIGUSR1);
    CHECK(got_usr1 == SIGUSR1, "signal handler runs (kill self)");
    sa.sa_handler = on_alrm; sigaction(SIGALRM, &sa, NULL);
    alarm(1);
    struct timespec ts = { 0, 1500000000 };
    nanosleep(&ts, NULL);
    CHECK(got_alrm == SIGALRM, "alarm delivers SIGALRM and interrupts sleep");
    sa.sa_handler = on_segv; sigaction(SIGSEGV, &sa, NULL);
    if (sigsetjmp(jb, 1) == 0) { *(volatile int *)16 = 1; CHECK(0, "SIGSEGV on a null write"); }
    else CHECK(1, "SIGSEGV handler and siglongjmp");
    c = fork();
    if (c == 0) { signal(SIGSEGV, SIG_DFL); *(volatile int *)0 = 1; _exit(0); }
    waitpid(c, &st, 0);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV, "unhandled SIGSEGV kills the child");

    /* threads */
    pthread_t th[4];
    for (int i = 0; i < 4; i++) pthread_create(&th[i], NULL, worker, NULL);
    for (int i = 0; i < 4; i++) pthread_join(th[i], NULL);
    CHECK(counter == 4000, "4 threads with a mutex");
    pthread_t wt;
    pthread_create(&wt, NULL, waiter, NULL);
    usleep(20000);
    pthread_mutex_lock(&mu); ready = 1; pthread_cond_signal(&cv); pthread_mutex_unlock(&mu);
    pthread_join(wt, NULL);
    CHECK(1, "condition variable wake");

    /* files */
    mkdir("/tmp/t1", 0755);
    int fd = open("/tmp/t1/a.txt", O_CREAT | O_RDWR | O_TRUNC, 0644);
    CHECK(fd >= 0, "open(O_CREAT)");
    CHECK(write(fd, "hello world", 11) == 11, "write");
    CHECK(lseek(fd, 6, SEEK_SET) == 6, "lseek");
    memset(buf, 0, sizeof(buf));
    CHECK(read(fd, buf, 5) == 5 && !strcmp(buf, "world"), "read after seek");
    CHECK(pwrite(fd, "H", 1, 0) == 1 && pread(fd, buf, 5, 0) == 5 && !memcmp(buf, "Hello", 5), "pread/pwrite");
    struct stat sb;
    CHECK(fstat(fd, &sb) == 0 && sb.st_size == 11 && S_ISREG(sb.st_mode), "fstat");
    CHECK(ftruncate(fd, 4) == 0 && fstat(fd, &sb) == 0 && sb.st_size == 4, "ftruncate");
    int d2 = dup2(fd, 20);
    CHECK(d2 == 20, "dup2");
    close(fd); close(d2);
    CHECK(rename("/tmp/t1/a.txt", "/tmp/t1/b.txt") == 0 && stat("/tmp/t1/b.txt", &sb) == 0, "rename");
    DIR *dir = opendir("/tmp/t1");
    int found = 0;
    struct dirent *de;
    while (dir && (de = readdir(dir))) if (!strcmp(de->d_name, "b.txt")) found = 1;
    if (dir) closedir(dir);
    CHECK(found, "opendir/readdir");
    CHECK(unlink("/tmp/t1/b.txt") == 0 && rmdir("/tmp/t1") == 0, "unlink/rmdir");
    CHECK(stat("/nonexistent", &sb) == -1 && errno == ENOENT, "ENOENT");
    CHECK(access("/disk/hello", X_OK) == 0, "access X_OK on a program");

    /* memory */
    char *a = mmap(0, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(a != MAP_FAILED, "mmap anonymous");
    a[0] = 1; a[(1 << 20) - 1] = 2;
    CHECK(a[0] == 1 && a[(1 << 20) - 1] == 2, "mmap touch");
    CHECK(mprotect(a, 4096, PROT_READ) == 0, "mprotect");
    sa.sa_handler = on_segv; sigaction(SIGSEGV, &sa, NULL);
    if (sigsetjmp(jb, 1) == 0) { a[0] = 9; CHECK(0, "write to a read-only page faults"); } else CHECK(1, "write to a read-only page faults");
    CHECK(munmap(a, 1 << 20) == 0, "munmap");
    char *sh = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    c = fork();
    if (c == 0) { sh[0] = 42; _exit(0); }
    waitpid(c, &st, 0);
    CHECK(sh[0] == 42, "shared anonymous memory across fork");
    char *big = mmap(0, 1ull << 36, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    CHECK(big != MAP_FAILED, "64 GiB reservation");
    fd = open("/disk/hello", O_RDONLY);
    char *fm = mmap(0, 4096, PROT_READ, MAP_PRIVATE, fd, 0);
    CHECK(fm != MAP_FAILED && !memcmp(fm, "\x7f" "ELF", 4), "mmap of a file");
    close(fd);

    /* proc and system */
    fd = open("/proc/self/maps", O_RDONLY);
    n = fd >= 0 ? read(fd, buf, sizeof(buf)) : -1;
    CHECK(n > 0, "/proc/self/maps readable");
    if (fd >= 0) close(fd);
    char exe[128] = {0};
    n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    CHECK(n > 0 && strstr(exe, "abitest"), "readlink /proc/self/exe");
    struct utsname u;
    CHECK(uname(&u) == 0 && !strcmp(u.machine, "x86_64"), "uname");
    CHECK(sysconf(_SC_NPROCESSORS_ONLN) >= 1, "sysconf processors");
    unsigned char rnd[16];
    CHECK(getrandom(rnd, 16, 0) == 16, "getrandom");
    struct timespec t1, t2;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    usleep(50000);
    clock_gettime(CLOCK_MONOTONIC, &t2);
    long ms = (t2.tv_sec - t1.tv_sec) * 1000 + (t2.tv_nsec - t1.tv_nsec) / 1000000;
    CHECK(ms >= 45 && ms < 200, "usleep and monotonic clock");
    CHECK(getuid() > 0, "runs as a normal user");

    /* poll on a pipe */
    int q[2];
    pipe(q);
    struct pollfd pf = { q[0], POLLIN, 0 };
    CHECK(poll(&pf, 1, 20) == 0, "poll times out when empty");
    write(q[1], "x", 1);
    CHECK(poll(&pf, 1, 20) == 1 && (pf.revents & POLLIN), "poll sees data");

    /* select */
    {
        fd_set rs; FD_ZERO(&rs); FD_SET(q[0], &rs);
        struct timeval tv = { 0, 20000 };
        CHECK(select(q[0] + 1, &rs, NULL, NULL, &tv) == 1 && FD_ISSET(q[0], &rs), "select sees data");
    }

    /* epoll with a pipe and an eventfd */
    {
        int ep = epoll_create1(0);
        int ev = eventfd(0, EFD_NONBLOCK);
        int p2[2];
        pipe(p2);
        struct epoll_event e1 = { .events = EPOLLIN, .data.fd = p2[0] };
        struct epoll_event e2 = { .events = EPOLLIN | EPOLLET, .data.fd = ev };
        CHECK(ep >= 0 && ev >= 0, "epoll_create1, eventfd");
        CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, p2[0], &e1) == 0 && epoll_ctl(ep, EPOLL_CTL_ADD, ev, &e2) == 0, "epoll_ctl add");
        struct epoll_event out[4];
        CHECK(epoll_wait(ep, out, 4, 20) == 0, "epoll_wait times out");
        uint64_t one = 5;
        write(ev, &one, 8);
        int n = epoll_wait(ep, out, 4, 100);
        CHECK(n == 1 && out[0].data.fd == ev, "epoll sees eventfd");
        n = epoll_wait(ep, out, 4, 20);
        CHECK(n == 0, "edge-triggered event is reported once");
        uint64_t got = 0;
        CHECK(read(ev, &got, 8) == 8 && got == 5, "eventfd counter");
        write(p2[1], "z", 1);
        n = epoll_wait(ep, out, 4, 100);
        CHECK(n == 1 && out[0].data.fd == p2[0], "epoll sees pipe data");
        write(p2[1], "z", 1);
        n = epoll_wait(ep, out, 4, 100);
        CHECK(n == 1, "level-triggered event repeats");
        CHECK(epoll_ctl(ep, EPOLL_CTL_DEL, p2[0], NULL) == 0, "epoll_ctl del");
        n = epoll_wait(ep, out, 4, 20);
        CHECK(n == 0, "removed fd is silent");
        close(ep); close(ev); close(p2[0]); close(p2[1]);
    }

    /* a blocking epoll_wait woken by another process */
    {
        int ep = epoll_create1(0), p3[2];
        pipe(p3);
        struct epoll_event e = { .events = EPOLLIN, .data.fd = p3[0] };
        epoll_ctl(ep, EPOLL_CTL_ADD, p3[0], &e);
        pid_t c = fork();
        if (c == 0) { usleep(60000); write(p3[1], "w", 1); _exit(0); }
        struct epoll_event out;
        int n = epoll_wait(ep, &out, 1, 2000);
        CHECK(n == 1 && out.data.fd == p3[0], "epoll_wait woken by a child");
        waitpid(c, NULL, 0);
        close(ep);
    }

    /* timerfd */
    {
        int tf = timerfd_create(CLOCK_MONOTONIC, 0);
        struct itimerspec its = { .it_interval = { 0, 0 }, .it_value = { 0, 40000000 } };
        CHECK(tf >= 0 && timerfd_settime(tf, 0, &its, NULL) == 0, "timerfd_settime");
        struct pollfd tp = { tf, POLLIN, 0 };
        CHECK(poll(&tp, 1, 5) == 0, "timerfd not yet expired");
        uint64_t exp = 0;
        CHECK(read(tf, &exp, 8) == 8 && exp == 1, "timerfd read blocks until expiry");
        struct itimerspec per = { .it_interval = { 0, 20000000 }, .it_value = { 0, 20000000 } };
        timerfd_settime(tf, 0, &per, NULL);
        usleep(110000);
        read(tf, &exp, 8);
        CHECK(exp >= 3 && exp <= 7, "periodic timerfd counts expirations");
        close(tf);
    }

    /* signalfd */
    {
        sigset_t m;
        sigemptyset(&m);
        sigaddset(&m, SIGUSR2);
        sigprocmask(SIG_BLOCK, &m, NULL);
        int sf = signalfd(-1, &m, 0);
        CHECK(sf >= 0, "signalfd");
        kill(getpid(), SIGUSR2);
        struct signalfd_siginfo si;
        struct pollfd sp = { sf, POLLIN, 0 };
        CHECK(poll(&sp, 1, 100) == 1, "signalfd becomes readable");
        CHECK(read(sf, &si, sizeof(si)) == sizeof(si) && si.ssi_signo == SIGUSR2, "signalfd delivers the signal");
        close(sf);
        sigprocmask(SIG_UNBLOCK, &m, NULL);
    }

    /* sockets: socketpair, descriptor passing */
    {
        int sv[2];
        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair stream");
        CHECK(write(sv[0], "hello", 5) == 5, "socket write");
        char b[16] = { 0 };
        CHECK(read(sv[1], b, sizeof(b)) == 5 && !memcmp(b, "hello", 5), "socket read");
        shutdown(sv[0], SHUT_WR);
        CHECK(read(sv[1], b, sizeof(b)) == 0, "shutdown(SHUT_WR) gives end of file");
        close(sv[0]); close(sv[1]);

        CHECK(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0, "socketpair dgram");
        send(sv[0], "one", 3, 0); send(sv[0], "three", 5, 0);
        CHECK(recv(sv[1], b, sizeof(b), 0) == 3 && recv(sv[1], b, sizeof(b), 0) == 5, "datagram boundaries kept");
        close(sv[0]); close(sv[1]);

        CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0, "socketpair seqpacket");
        send(sv[0], "abcdef", 6, 0);
        CHECK(recv(sv[1], b, 3, 0) == 3, "seqpacket truncates to the buffer");
        close(sv[0]);
        CHECK(recv(sv[1], b, sizeof(b), 0) == 0, "seqpacket sees the peer close");
        close(sv[1]);

        /* pass a descriptor */
        socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
        int pp[2];
        pipe(pp);
        char ctl[CMSG_SPACE(sizeof(int))] = { 0 };
        struct iovec io = { "x", 1 };
        struct msghdr mh = { 0 };
        mh.msg_iov = &io; mh.msg_iovlen = 1; mh.msg_control = ctl; mh.msg_controllen = sizeof(ctl);
        struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
        cm->cmsg_level = SOL_SOCKET; cm->cmsg_type = SCM_RIGHTS; cm->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cm), &pp[1], sizeof(int));
        CHECK(sendmsg(sv[0], &mh, 0) == 1, "sendmsg with a descriptor");
        close(pp[1]);
        char rb[2]; char rc[CMSG_SPACE(sizeof(int))] = { 0 };
        struct iovec ri = { rb, 1 };
        struct msghdr rm = { 0 };
        rm.msg_iov = &ri; rm.msg_iovlen = 1; rm.msg_control = rc; rm.msg_controllen = sizeof(rc);
        CHECK(recvmsg(sv[1], &rm, 0) == 1, "recvmsg");
        cm = CMSG_FIRSTHDR(&rm);
        int got = -1;
        if (cm && cm->cmsg_type == SCM_RIGHTS) memcpy(&got, CMSG_DATA(cm), sizeof(int));
        CHECK(got >= 0, "the descriptor arrived");
        CHECK(write(got, "via", 3) == 3 && read(pp[0], b, 3) == 3 && !memcmp(b, "via", 3), "the passed pipe works");
        close(got); close(pp[0]); close(sv[0]); close(sv[1]);
    }

    /* a named unix socket with a server child */
    {
        unlink("/tmp/abitest.sock");
        int ls = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un ua = { .sun_family = AF_UNIX };
        strcpy(ua.sun_path, "/tmp/abitest.sock");
        CHECK(bind(ls, (struct sockaddr *)&ua, sizeof(ua)) == 0, "bind unix path");
        CHECK(listen(ls, 4) == 0, "listen unix");
        pid_t c = fork();
        if (c == 0) {
            int s = socket(AF_UNIX, SOCK_STREAM, 0);
            if (connect(s, (struct sockaddr *)&ua, sizeof(ua)) != 0) _exit(1);
            char m[8] = { 0 };
            if (read(s, m, 4) != 4) _exit(2);
            write(s, m, 4);
            _exit(0);
        }
        int a = accept(ls, NULL, NULL);
        CHECK(a >= 0, "accept unix");
        write(a, "ping", 4);
        char m[8] = { 0 };
        CHECK(read(a, m, 4) == 4 && !memcmp(m, "ping", 4), "unix echo through a child");
        int st = 0;
        waitpid(c, &st, 0);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "the child connected and finished");
        int cs = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un none = { .sun_family = AF_UNIX };
        strcpy(none.sun_path, "/tmp/abitest.nope");
        CHECK(connect(cs, (struct sockaddr *)&none, sizeof(none)) < 0 && errno == ENOENT, "connect to a missing path: ENOENT");
        struct ucred uc; socklen_t ul = sizeof(uc);
        CHECK(getsockopt(a, SOL_SOCKET, SO_PEERCRED, &uc, &ul) == 0 && uc.pid > 0, "SO_PEERCRED");
        close(cs); close(a); close(ls);
        unlink("/tmp/abitest.sock");
    }

    /* TCP inside the host */
    {
        int ls = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(34567), .sin_addr.s_addr = htonl(INADDR_ANY) };
        CHECK(bind(ls, (struct sockaddr *)&sa, sizeof(sa)) == 0, "bind tcp");
        CHECK(listen(ls, 8) == 0, "listen tcp");
        struct sockaddr_in got; socklen_t gl = sizeof(got);
        CHECK(getsockname(ls, (struct sockaddr *)&got, &gl) == 0 && ntohs(got.sin_port) == 34567, "getsockname");
        int cs = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int rc = connect(cs, (struct sockaddr *)&sa, sizeof(sa));
        CHECK(rc == 0 || errno == EINPROGRESS, "non-blocking connect to localhost");
        struct pollfd lp = { ls, POLLIN, 0 };
        CHECK(poll(&lp, 1, 500) == 1, "listener becomes readable");
        struct sockaddr_in peer; socklen_t pl = sizeof(peer);
        int a = accept(ls, (struct sockaddr *)&peer, &pl);
        CHECK(a >= 0 && peer.sin_addr.s_addr == htonl(INADDR_LOOPBACK), "accept tcp");
        struct pollfd cp = { cs, POLLOUT, 0 };
        CHECK(poll(&cp, 1, 500) == 1 && (cp.revents & POLLOUT), "client connected (POLLOUT)");
        int e = -1; socklen_t el = sizeof(e);
        getsockopt(cs, SOL_SOCKET, SO_ERROR, &e, &el);
        CHECK(e == 0, "SO_ERROR is 0");
        char big[100000];
        for (size_t i = 0; i < sizeof(big); i++) big[i] = (char)(i * 7);
        pid_t c = fork();
        if (c == 0) {
            close(ls); close(cs);
            size_t sent = 0;
            while (sent < sizeof(big)) { ssize_t k = write(a, big + sent, sizeof(big) - sent); if (k <= 0) _exit(1); sent += (size_t)k; }
            _exit(0);
        }
        close(a);
        a = -1;
        char rbuf[100000];
        size_t have = 0;
        fcntl(cs, F_SETFL, fcntl(cs, F_GETFL) & ~O_NONBLOCK);
        while (have < sizeof(rbuf)) { ssize_t k = read(cs, rbuf + have, sizeof(rbuf) - have); if (k <= 0) break; have += (size_t)k; }
        CHECK(have == sizeof(big) && !memcmp(big, rbuf, sizeof(big)), "100000 bytes over loopback tcp");
        waitpid(c, NULL, 0);
        CHECK(read(cs, rbuf, 1) == 0, "end of file after the peer closed");
        int ns = socket(AF_INET, SOCK_STREAM, 0);
        sa.sin_port = htons(34568);
        CHECK(connect(ns, (struct sockaddr *)&sa, sizeof(sa)) < 0 && errno == ECONNREFUSED, "connect to a closed port: ECONNREFUSED");
        close(ns); close(cs); close(ls);
    }

    /* UDP on loopback */
    {
        int a = socket(AF_INET, SOCK_DGRAM, 0), b = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(34570), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
        CHECK(bind(a, (struct sockaddr *)&sa, sizeof(sa)) == 0, "bind udp");
        CHECK(sendto(b, "dgram", 5, 0, (struct sockaddr *)&sa, sizeof(sa)) == 5, "sendto udp");
        char m[16]; struct sockaddr_in from; socklen_t fl = sizeof(from);
        CHECK(recvfrom(a, m, sizeof(m), 0, (struct sockaddr *)&from, &fl) == 5 && !memcmp(m, "dgram", 5), "recvfrom udp");
        CHECK(from.sin_port != 0, "the sender's port is reported");
        sendto(a, "back", 4, 0, (struct sockaddr *)&from, fl);
        CHECK(recv(b, m, sizeof(m), 0) == 4, "reply to the sender");
        close(a); close(b);
    }

    /* the real network: DNS over UDP and an HTTP request over TCP */
    {
        FILE *rf = fopen("/etc/resolv.conf", "r");
        char line[128], ns[64] = "";
        while (rf && fgets(line, sizeof(line), rf)) if (!strncmp(line, "nameserver ", 11)) { sscanf(line + 11, "%63s", ns); break; }
        if (rf) fclose(rf);
        CHECK(ns[0] != 0, "resolv.conf names a server");
        if (ns[0]) {
            unsigned char q[512] = { 0x12, 0x34, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0 };
            int n = 12;
            const char *host = "example.com";
            const char *p = host;
            while (*p) {
                const char *d = strchr(p, '.');
                int l = d ? (int)(d - p) : (int)strlen(p);
                q[n++] = (unsigned char)l;
                memcpy(q + n, p, (size_t)l); n += l;
                p += l + (d ? 1 : 0);
                if (!d) break;
            }
            q[n++] = 0; q[n++] = 0; q[n++] = 1; q[n++] = 0; q[n++] = 1;
            int u = socket(AF_INET, SOCK_DGRAM, 0);
            struct sockaddr_in dns = { .sin_family = AF_INET, .sin_port = htons(53) };
            inet_pton(AF_INET, ns, &dns.sin_addr);
            sendto(u, q, (size_t)n, 0, (struct sockaddr *)&dns, sizeof(dns));
            struct pollfd up = { u, POLLIN, 0 };
            unsigned char ans[512];
            int pr = poll(&up, 1, 4000);
            ssize_t an = pr == 1 ? recv(u, ans, sizeof(ans), 0) : -1;
            if (pr == 0) { printf("skip  DNS and HTTP over the real network (no answer from %s within 4 s)\n", ns); fflush(stdout); close(u); goto net_done; }
            CHECK(an > 12 && ans[0] == 0x12 && ans[1] == 0x34, "UDP round trip to the DNS server");
            if (an > 12 && (ans[3] & 15) != 0) { printf("skip  name lookup and HTTP (the resolver answered with code %d: no internet here)\n", ans[3] & 15); fflush(stdout); close(u); goto net_done; }
            char ip[16] = "";
            if (an > 12) {
                int ac = (ans[6] << 8) | ans[7];
                int o = n;
                for (int i = 0; i < ac && o + 12 <= an; i++) {
                    if ((ans[o] & 0xC0) == 0xC0) o += 2; else { while (ans[o]) o += ans[o] + 1; o++; }
                    int type = (ans[o] << 8) | ans[o + 1], rl = (ans[o + 8] << 8) | ans[o + 9];
                    if (type == 1 && rl == 4) { snprintf(ip, sizeof(ip), "%d.%d.%d.%d", ans[o + 10], ans[o + 11], ans[o + 12], ans[o + 13]); break; }
                    o += 10 + rl;
                }
            }
            close(u);
            CHECK(ip[0] != 0, "an A record for example.com");
            if (ip[0]) {
                int t = socket(AF_INET, SOCK_STREAM, 0);
                struct sockaddr_in web = { .sin_family = AF_INET, .sin_port = htons(80) };
                inet_pton(AF_INET, ip, &web.sin_addr);
                CHECK(connect(t, (struct sockaddr *)&web, sizeof(web)) == 0, "TCP connect over the network");
                const char *req = "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n";
                write(t, req, strlen(req));
                char resp[256] = { 0 };
                ssize_t rn = read(t, resp, sizeof(resp) - 1);
                CHECK(rn > 12 && !strncmp(resp, "HTTP/1.", 7), "an HTTP answer");
                close(t);
            }
        }
    }
net_done:;

    printf("abitest done, %d failure(s)\n", fails);
    return fails ? 1 : 0;
}

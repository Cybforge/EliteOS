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

    printf("abitest done, %d failure(s)\n", fails);
    return fails ? 1 : 0;
}

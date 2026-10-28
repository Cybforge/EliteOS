#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
extern char **environ;
int main(int argc, char **argv)
{
    printf("hello from a glibc static program, pid %d, argc %d\n", getpid(), argc);
    for (int i = 0; i < argc; i++) printf("  argv[%d] = %s\n", i, argv[i]);
    int n = 0;
    for (char **e = environ; *e && n < 4; e++, n++) printf("  env: %s\n", *e);
    char *p = malloc(10 * 1024 * 1024);
    memset(p, 7, 10 * 1024 * 1024);
    printf("malloc 10 MiB ok, p[9999999]=%d\n", p[9999999]);
    free(p);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    printf("monotonic %ld.%09ld\n", (long)ts.tv_sec, ts.tv_nsec);
    double x = 0;
    for (int i = 1; i < 1000; i++) x += 1.0 / i;
    printf("harmonic(999) = %.6f\n", x);
    return 3;
}

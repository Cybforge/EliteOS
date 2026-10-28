#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <dlfcn.h>
#include <unistd.h>
#include <locale.h>
#include <time.h>
#include <sys/wait.h>
static void *w(void *a) { return (void *)(long)(*(int *)a * 2); }
static int cmp(const void *a, const void *b) { return *(int *)a - *(int *)b; }
int main(int argc, char **argv)
{
    printf("dynamic hello from %s (%d args)\n", argv[0], argc);
    if (argc > 1) { printf("  (child) args ok\n"); return 0; }
    int v[] = { 5, 3, 9, 1 };
    qsort(v, 4, sizeof(int), cmp);
    printf("sorted: %d %d %d %d\n", v[0], v[1], v[2], v[3]);
    printf("sqrt(2)=%.6f\n", sqrt(2.0));
    pthread_t t; int x = 21;
    pthread_create(&t, NULL, w, &x);
    void *rv; pthread_join(t, &rv);
    printf("thread returned %ld\n", (long)rv);
    void *h = dlopen("libm.so.6", RTLD_NOW);
    printf("dlopen libm: %s\n", h ? "ok" : dlerror());
    if (h) { double (*c)(double) = dlsym(h, "cbrt"); printf("cbrt(27)=%.1f\n", c ? c(27.0) : -1); }
    setlocale(LC_ALL, "C");
    char *m = malloc(1 << 20); memset(m, 7, 1 << 20); free(m);
    pid_t p = fork();
    if (p == 0) { execl(argv[0], argv[0], "child", NULL); _exit(9); }
    int st; waitpid(p, &st, 0);
    printf("child status %d\n", WEXITSTATUS(st));
    FILE *f = fopen("/proc/self/maps", "r"); int lines = 0; char b[256];
    while (f && fgets(b, sizeof b, f)) lines++;
    printf("maps lines %d\n", lines);
    return 0;
}

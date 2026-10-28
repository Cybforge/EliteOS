/* primes: a sieve of Eratosthenes in user space. */
#include <zenith.h>

int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 5000000;
    if (n < 10) n = 10;
    char *composite = calloc((size_t)n + 1, 1);
    if (!composite) { printf("out of memory\n"); return 1; }
    unsigned long t0 = uptime();
    int count = 0;
    for (int i = 2; i <= n; i++) {
        if (composite[i]) continue;
        count++;
        for (long j = (long)i * i; j <= n; j += i) composite[j] = 1;
    }
    unsigned long t = uptime() - t0;
    printf("%d primes below %d, found in %lu ms\n", count, n, t);
    int shown = 0;
    printf("largest:");
    for (int i = n; i > 1 && shown < 5; i--) if (!composite[i]) { printf(" %d", i); shown++; }
    printf("\n");
    free(composite);
    return 0;
}

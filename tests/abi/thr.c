#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
static void *f(void *a) { printf("in thread\n"); fflush(stdout); return (void *)5; }
int main(void)
{
    printf("main start\n"); fflush(stdout);
    pthread_t t;
    int r = pthread_create(&t, 0, f, 0);
    printf("created r=%d\n", r); fflush(stdout);
    void *v = 0;
    r = pthread_join(t, &v);
    printf("joined r=%d v=%ld\n", r, (long)v); fflush(stdout);
    return 0;
}

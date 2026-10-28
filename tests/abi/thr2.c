#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
static int counter;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static void *worker(void *a)
{
    for (int i = 0; i < 1000; i++) { pthread_mutex_lock(&mu); counter++; pthread_mutex_unlock(&mu); }
    printf("worker done\n"); fflush(stdout);
    return a;
}
int main(void)
{
    pthread_t th[4];
    for (int i = 0; i < 4; i++) { pthread_create(&th[i], NULL, worker, NULL); printf("created %d\n", i); fflush(stdout); }
    for (int i = 0; i < 4; i++) { pthread_join(th[i], NULL); printf("joined %d\n", i); fflush(stdout); }
    printf("counter %d\n", counter);
    return 0;
}

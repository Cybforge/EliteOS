/*
 * parallel_for: spread independent work items over all CPU cores using a
 * pool of worker threads (one per extra core). The caller works too.
 */
#include <kernel.h>
#include <sched.h>
#include <cpu.h>
#include <vfs.h>
#include <parallel.h>

struct job {
    void (*fn)(int i, void *ctx);
    void *ctx;
    int n;
    int next;
    int done;
    int active;     /* workers attached to this job, under job_lock */
};

static mutex_t pool_mtx = MUTEX_INIT("parallel");
static spinlock_t job_lock = SPINLOCK_INIT("parallel-job");
static struct job *jobp;
static uint64_t generation;
static int nworkers;
static char job_chan, done_chan;

static void run_items(struct job *j)
{
    for (;;) {
        int i = __atomic_fetch_add(&j->next, 1, __ATOMIC_ACQ_REL);
        if (i >= j->n) break;
        j->fn(i, j->ctx);
        if (__atomic_add_fetch(&j->done, 1, __ATOMIC_ACQ_REL) == j->n) sched_wake(&done_chan);
    }
}

static int worker(void *arg)
{
    UNUSED(arg);
    uint64_t seen = 0;
    for (;;) {
        spin_lock(&job_lock);
        while (generation == seen) sched_wait(&job_chan, &job_lock, 0);
        seen = generation;
        struct job *j = jobp;
        if (j) j->active++;
        spin_unlock(&job_lock);
        if (!j) continue;
        run_items(j);
        spin_lock(&job_lock);
        j->active--;
        spin_unlock(&job_lock);
        sched_wake(&done_chan);
    }
    return 0;
}

int parallel_workers(void) { return nworkers + 1; }

void parallel_for(int n, void (*fn)(int i, void *ctx), void *ctx)
{
    if (n <= 0) return;
    mutex_lock(&pool_mtx);
    if (!nworkers && ncpus > 1) {
        for (int i = 0; i < ncpus - 1; i++) thread_create_ex("worker", worker, NULL, 0, -1, NULL, 128 * 1024);
        nworkers = ncpus - 1;
    }
    struct job j = { fn, ctx, n, 0, 0, 0 };
    spin_lock(&job_lock);
    jobp = &j;
    generation++;
    spin_unlock(&job_lock);
    sched_wake(&job_chan);
    run_items(&j);
    spin_lock(&job_lock);
    while (__atomic_load_n(&j.done, __ATOMIC_ACQUIRE) < n || j.active > 0)
        sched_wait(&done_chan, &job_lock, 5);
    jobp = NULL;
    spin_unlock(&job_lock);
    mutex_unlock(&pool_mtx);
}

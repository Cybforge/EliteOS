/* Sleeping mutex built on the scheduler's wait channels. */
#include <vfs.h>
#include <sched.h>

void mutex_lock(mutex_t *m)
{
    spin_lock(&m->lk);
    while (m->locked) sched_wait(m, &m->lk, 0);
    m->locked = 1;
    m->owner = thread_current();
    spin_unlock(&m->lk);
}

bool mutex_trylock(mutex_t *m)
{
    bool ok = false;
    spin_lock(&m->lk);
    if (!m->locked) {
        m->locked = 1;
        m->owner = thread_current();
        ok = true;
    }
    spin_unlock(&m->lk);
    return ok;
}

void mutex_unlock(mutex_t *m)
{
    spin_lock(&m->lk);
    m->locked = 0;
    m->owner = NULL;
    sched_wake(m);
    spin_unlock(&m->lk);
}

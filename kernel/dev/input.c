/* Raw input ring: drivers push from IRQ context, the window manager pops. */
#include <kernel.h>
#include <input.h>
#include <spinlock.h>
#include <sched.h>

#define RING 512

static struct raw_input ring[RING];
static unsigned head, tail;
static spinlock_t lock = SPINLOCK_INIT("input");
static char chan;

void input_init(void) {}

void *input_chan(void) { return &chan; }

void input_push(const struct raw_input *ev)
{
    spin_lock(&lock);
    /* coalesce consecutive absolute moves with the same buttons */
    if (ev->type == RAW_MOUSE_ABS && head != tail) {
        struct raw_input *last = &ring[(head - 1) % RING];
        if (last->type == RAW_MOUSE_ABS && last->buttons == ev->buttons && !last->wheel && !ev->wheel) {
            *last = *ev;
            spin_unlock(&lock);
            sched_wake(&chan);
            return;
        }
    }
    if (head - tail < RING) {
        ring[head % RING] = *ev;
        head++;
    }
    spin_unlock(&lock);
    sched_wake(&chan);
}

bool input_pop(struct raw_input *ev)
{
    spin_lock(&lock);
    bool ok = head != tail;
    if (ok) {
        *ev = ring[tail % RING];
        tail++;
    }
    spin_unlock(&lock);
    return ok;
}

#include <spinlock.h>
#include <cpu.h>
#include <x86.h>

void push_cli(void)
{
    uint64_t f = read_rflags();
    cli();
    struct cpu *c = this_cpu();
    if (c->ncli++ == 0)
        c->intena = (f & RFLAGS_IF) != 0;
}

void pop_cli(void)
{
    if (read_rflags() & RFLAGS_IF)
        panic("pop_cli: interrupts enabled");
    struct cpu *c = this_cpu();
    if (--c->ncli < 0)
        panic("pop_cli: unbalanced");
    if (c->ncli == 0 && c->intena)
        sti();
}

void spin_init(spinlock_t *l, const char *name)
{
    l->locked = 0;
    l->name = name;
    l->owner = NULL;
}

bool spin_holding(spinlock_t *l)
{
    bool r;
    push_cli();
    r = l->locked && l->owner == this_cpu();
    pop_cli();
    return r;
}

void spin_lock(spinlock_t *l)
{
    push_cli();
    struct cpu *c = this_cpu();
    if (l->locked && l->owner == c)
        panic("spinlock '%s': recursive acquire on cpu %d", l->name, c->id);
    uint64_t spins = 0;
    while (__atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE)) {
        while (l->locked) {
            cpu_relax();
            if (++spins == 2000000000ull)
                panic("spinlock '%s': stuck (held by cpu %d)", l->name, l->owner ? l->owner->id : -1);
        }
    }
    l->owner = c;
}

bool spin_trylock(spinlock_t *l)
{
    push_cli();
    if (__atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE)) {
        pop_cli();
        return false;
    }
    l->owner = this_cpu();
    return true;
}

void spin_unlock(spinlock_t *l)
{
    if (!l->locked || l->owner != this_cpu())
        panic("spinlock '%s': release by non-owner", l->name);
    l->owner = NULL;
    __atomic_store_n(&l->locked, 0, __ATOMIC_RELEASE);
    pop_cli();
}

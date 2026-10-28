#ifndef ZENITH_SPINLOCK_H
#define ZENITH_SPINLOCK_H

#include <kernel.h>

struct cpu;

typedef struct {
    volatile uint32_t locked;
    const char *name;
    struct cpu *owner;
} spinlock_t;

#define SPINLOCK_INIT(n) { 0, (n), 0 }

void spin_init(spinlock_t *l, const char *name);
void spin_lock(spinlock_t *l);
void spin_unlock(spinlock_t *l);
bool spin_trylock(spinlock_t *l);
bool spin_holding(spinlock_t *l);

/* nestable interrupt disable, per CPU */
void push_cli(void);
void pop_cli(void);

#endif

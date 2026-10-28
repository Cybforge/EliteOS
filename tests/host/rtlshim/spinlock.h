#pragma once
typedef struct { int v; } spinlock_t;
#define SPINLOCK_INIT(n) { 0 }
static inline void spin_lock(spinlock_t *l) { (void)l; }
static inline void spin_unlock(spinlock_t *l) { (void)l; }

#pragma once
typedef struct { int v; const char *name; } spinlock_t;
#define SPINLOCK_INIT(n) { 0, n }
static inline void spin_lock(spinlock_t *l) { (void)l; }
static inline void spin_unlock(spinlock_t *l) { (void)l; }

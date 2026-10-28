#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define UNUSED(x) (void)(x)
#define PACKED __attribute__((packed))
static inline void *kzalloc(size_t n) { return calloc(1, n); }
void klog(const char *fmt, ...);

#pragma once
#include <stddef.h>
void *kmalloc(size_t n); void *kzalloc(size_t n); void *krealloc(void *p, size_t n); void kfree(void *p);

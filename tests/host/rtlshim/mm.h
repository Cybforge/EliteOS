#pragma once
#include "kernel.h"
#define PAGE_SIZE 4096ul
void *vmm_map_mmio(uint64_t phys, uint64_t size);

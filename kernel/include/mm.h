#ifndef ZENITH_MM_H
#define ZENITH_MM_H

#include <kernel.h>
#include <bootinfo.h>

#define PAGE_SIZE 4096ul

#define P2V(pa) ((void *)((uint64_t)(pa) + HHDM_BASE))
#define V2P(va) ((uint64_t)(va) - HHDM_BASE)

#define VMALLOC_BASE 0xFFFFC00000000000ull
#define VMALLOC_END  0xFFFFE00000000000ull

#define USER_BASE    0x0000000000400000ull
#define USER_TOP     0x00007FFFFFFFF000ull
#define USER_STACK_TOP  0x00007FFFFFF00000ull
#define USER_STACK_SIZE (1024 * 1024)

/* page table flags */
#define PTE_P    (1ull << 0)
#define PTE_W    (1ull << 1)
#define PTE_U    (1ull << 2)
#define PTE_PWT  (1ull << 3)
#define PTE_PCD  (1ull << 4)
#define PTE_A    (1ull << 5)
#define PTE_D    (1ull << 6)
#define PTE_PS   (1ull << 7)
#define PTE_G    (1ull << 8)
#define PTE_OWNED (1ull << 9)       /* available bit: page is owned by the address space */
#define PTE_NX   (1ull << 63)
#define PTE_ADDR 0x000FFFFFFFFFF000ull

#define PTE_WC   PTE_PWT               /* PAT index 1 */
#define PTE_UC   (PTE_PWT | PTE_PCD)   /* PAT index 3 */

/* pmm.c */
void     pmm_init(struct bootinfo *bi);
uint64_t pmm_alloc(void);                 /* one zeroed page, physical addr, 0 on OOM */
uint64_t pmm_alloc_contig(size_t pages);  /* zeroed */
uint64_t pmm_alloc_below(uint64_t limit); /* zeroed page below limit */
uint64_t pmm_alloc_contig_below(size_t pages, uint64_t limit);
void     pmm_free(uint64_t pa);
void     pmm_free_contig(uint64_t pa, size_t pages);
uint64_t pmm_total_pages(void);
uint64_t pmm_free_pages(void);
void     pmm_reclaim_acpi(void);

/* vmm.c */
typedef struct addrspace {
    uint64_t pml4_phys;
} addrspace_t;

extern addrspace_t kernel_space;

void     vmm_init(struct bootinfo *bi);
bool     vmm_map(addrspace_t *as, uint64_t va, uint64_t pa, uint64_t flags);
void     vmm_unmap(addrspace_t *as, uint64_t va, bool free_page);
uint64_t vmm_translate(addrspace_t *as, uint64_t va);        /* 0 if unmapped */
uint64_t vmm_pte_flags(addrspace_t *as, uint64_t va);
addrspace_t *vmm_create_space(void);
void     vmm_destroy_space(addrspace_t *as);
void     vmm_switch(addrspace_t *as);
void     vmm_set_cache(uint64_t phys, uint64_t size, uint64_t mode_flags);
void    *vmm_map_mmio(uint64_t phys, uint64_t size);
void    *vmm_map_mmio_wc(uint64_t phys, uint64_t size);
void     vmm_identity_low(bool on);
bool     vmm_user_ok(addrspace_t *as, uint64_t va, uint64_t len, bool write);

void    *vmalloc_pages(size_t pages);      /* zeroed, not physically contiguous */
void     vfree_pages(void *p, size_t pages);

/* heap.c */
void  heap_init(void);
void *kmalloc(size_t size);
void *kzalloc(size_t size);
void *kcalloc(size_t n, size_t size);
void *krealloc(void *p, size_t size);
void  kfree(void *p);
size_t heap_used(void);

#endif

/*
 * Virtual memory: kernel page tables, per-process address spaces,
 * vmalloc area for large kernel allocations.
 */
#include <mm.h>
#include <cpu.h>
#include <x86.h>
#include <spinlock.h>

addrspace_t kernel_space;

/* Kernel PML4 lives in .bss so that it is below 4 GiB (the AP trampoline
 * loads CR3 from 32-bit code). */
static uint64_t kernel_pml4[512] __attribute__((aligned(4096)));
static uint64_t hhdm_size;
static spinlock_t vmm_lock = SPINLOCK_INIT("vmm");
static uint64_t vmalloc_next = VMALLOC_BASE;

extern char __text_start[], __text_end[], __rodata_start[], __rodata_end[];
extern char __data_start[], __kernel_end[];

static uint64_t kva_to_phys_base;   /* phys = va - KERNEL_VBASE + kernel_phys */

static inline uint64_t kvirt_to_phys(void *p)
{
    uint64_t va = (uint64_t)p;
    if (va >= KERNEL_VBASE) return va - KERNEL_VBASE + kva_to_phys_base;
    return V2P(va);
}

static uint64_t *table_next(uint64_t *table, unsigned idx, bool create, bool user)
{
    uint64_t e = table[idx];
    if (e & PTE_P) {
        if (user && !(e & PTE_U)) table[idx] = e | PTE_U;
        return P2V(e & PTE_ADDR);
    }
    if (!create) return NULL;
    uint64_t pa = pmm_alloc();
    if (!pa) return NULL;
    table[idx] = pa | PTE_P | PTE_W | (user ? PTE_U : 0);
    return P2V(pa);
}

static uint64_t *walk(addrspace_t *as, uint64_t va, bool create)
{
    bool user = va < 0x0000800000000000ull;
    uint64_t *pml4 = P2V(as->pml4_phys);
    uint64_t *pdpt = table_next(pml4, (va >> 39) & 511, create, user);
    if (!pdpt) return NULL;
    uint64_t *pd = table_next(pdpt, (va >> 30) & 511, create, user);
    if (!pd) return NULL;
    if (pd[(va >> 21) & 511] & PTE_PS) return NULL;   /* inside a 2 MiB mapping */
    uint64_t *pt = table_next(pd, (va >> 21) & 511, create, user);
    if (!pt) return NULL;
    return &pt[(va >> 12) & 511];
}

bool vmm_map(addrspace_t *as, uint64_t va, uint64_t pa, uint64_t flags)
{
    spin_lock(&vmm_lock);
    uint64_t *pte = walk(as, va, true);
    if (!pte) { spin_unlock(&vmm_lock); return false; }
    *pte = (pa & PTE_ADDR) | flags | PTE_P;
    spin_unlock(&vmm_lock);
    invlpg(va);
    return true;
}

void vmm_unmap(addrspace_t *as, uint64_t va, bool free_page)
{
    spin_lock(&vmm_lock);
    uint64_t *pte = walk(as, va, false);
    uint64_t old = 0;
    if (pte) { old = *pte; *pte = 0; }
    spin_unlock(&vmm_lock);
    invlpg(va);
    if (free_page && (old & PTE_P)) pmm_free(old & PTE_ADDR);
}

uint64_t vmm_translate(addrspace_t *as, uint64_t va)
{
    uint64_t *pml4 = P2V(as->pml4_phys);
    uint64_t e = pml4[(va >> 39) & 511];
    if (!(e & PTE_P)) return 0;
    uint64_t *pdpt = P2V(e & PTE_ADDR);
    e = pdpt[(va >> 30) & 511];
    if (!(e & PTE_P)) return 0;
    if (e & PTE_PS) return (e & 0x000FFFFFC0000000ull) + (va & 0x3FFFFFFF);
    uint64_t *pd = P2V(e & PTE_ADDR);
    e = pd[(va >> 21) & 511];
    if (!(e & PTE_P)) return 0;
    if (e & PTE_PS) return (e & 0x000FFFFFFFE00000ull) + (va & 0x1FFFFF);
    uint64_t *pt = P2V(e & PTE_ADDR);
    e = pt[(va >> 12) & 511];
    if (!(e & PTE_P)) return 0;
    return (e & PTE_ADDR) + (va & 0xFFF);
}

uint64_t vmm_pte_flags(addrspace_t *as, uint64_t va)
{
    uint64_t *pte = walk(as, va, false);
    return pte ? (*pte & ~PTE_ADDR) : 0;
}

bool vmm_user_ok(addrspace_t *as, uint64_t va, uint64_t len, bool write)
{
    if (!len) return true;
    if (va < USER_BASE || va + len < va || va + len > USER_TOP) return false;
    for (uint64_t p = ALIGN_DOWN(va, PAGE_SIZE); p < va + len; p += PAGE_SIZE) {
        uint64_t f = vmm_pte_flags(as, p);
        if (!(f & PTE_P) || !(f & PTE_U)) return false;
        if (write && !(f & PTE_W)) return false;
    }
    return true;
}

/* ------------------------------------------------------------------------
 * address spaces
 * ---------------------------------------------------------------------- */

addrspace_t *vmm_create_space(void)
{
    addrspace_t *as = kzalloc(sizeof(*as));
    if (!as) return NULL;
    as->pml4_phys = pmm_alloc();
    if (!as->pml4_phys) { kfree(as); return NULL; }
    uint64_t *pml4 = P2V(as->pml4_phys);
    for (int i = 256; i < 512; i++) pml4[i] = kernel_pml4[i];
    return as;
}

void vmm_destroy_space(addrspace_t *as)
{
    uint64_t *pml4 = P2V(as->pml4_phys);
    for (int i = 0; i < 256; i++) {
        if (!(pml4[i] & PTE_P)) continue;
        uint64_t *pdpt = P2V(pml4[i] & PTE_ADDR);
        for (int j = 0; j < 512; j++) {
            if (!(pdpt[j] & PTE_P)) continue;
            uint64_t *pd = P2V(pdpt[j] & PTE_ADDR);
            for (int k = 0; k < 512; k++) {
                if (!(pd[k] & PTE_P) || (pd[k] & PTE_PS)) continue;
                uint64_t *pt = P2V(pd[k] & PTE_ADDR);
                for (int l = 0; l < 512; l++)
                    if ((pt[l] & PTE_P) && (pt[l] & PTE_OWNED)) pmm_free(pt[l] & PTE_ADDR);
                pmm_free(pd[k] & PTE_ADDR);
            }
            pmm_free(pdpt[j] & PTE_ADDR);
        }
        pmm_free(pml4[i] & PTE_ADDR);
    }
    pmm_free(as->pml4_phys);
    kfree(as);
}

void vmm_switch(addrspace_t *as)
{
    if (read_cr3() != as->pml4_phys) write_cr3(as->pml4_phys);
}

/* ------------------------------------------------------------------------
 * caching attributes inside the direct map
 * ---------------------------------------------------------------------- */

void vmm_set_cache(uint64_t phys, uint64_t size, uint64_t mode)
{
    uint64_t start = ALIGN_DOWN(phys, 0x200000);
    uint64_t end = ALIGN_UP(phys + size, 0x200000);
    spin_lock(&vmm_lock);
    for (uint64_t pa = start; pa < end && pa < hhdm_size; pa += 0x200000) {
        uint64_t va = HHDM_BASE + pa;
        uint64_t *pdpt = P2V(kernel_pml4[(va >> 39) & 511] & PTE_ADDR);
        uint64_t *pd = P2V(pdpt[(va >> 30) & 511] & PTE_ADDR);
        uint64_t *e = &pd[(va >> 21) & 511];
        *e = (*e & ~(PTE_PWT | PTE_PCD)) | mode;
        invlpg(va);
    }
    spin_unlock(&vmm_lock);
}

static void *map_mmio(uint64_t phys, uint64_t size, uint64_t cache)
{
    if (phys + size <= hhdm_size) {
        vmm_set_cache(phys, size, cache);
        return P2V(phys);
    }
    /* above the direct map: give it its own window in the vmalloc area */
    uint64_t off = phys & 0xFFF;
    uint64_t pages = ALIGN_UP(size + off, PAGE_SIZE) / PAGE_SIZE;
    uint64_t va = __atomic_fetch_add(&vmalloc_next, (pages + 1) * PAGE_SIZE, __ATOMIC_RELAXED);
    for (uint64_t i = 0; i < pages; i++)
        vmm_map(&kernel_space, va + i * PAGE_SIZE, ALIGN_DOWN(phys, PAGE_SIZE) + i * PAGE_SIZE,
                PTE_W | PTE_G | PTE_NX | cache);
    return (void *)(va + off);
}

void *vmm_map_mmio(uint64_t phys, uint64_t size) { return map_mmio(phys, size, PTE_UC); }

/* write-combined: for framebuffers */
void *vmm_map_mmio_wc(uint64_t phys, uint64_t size) { return map_mmio(phys, size, PTE_WC); }

/* Identity map of the low 2 MiB, needed only while APs start up. */
void vmm_identity_low(bool on)
{
    static uint64_t pdpt_phys, pd_phys;
    if (on) {
        if (!pdpt_phys) { pdpt_phys = pmm_alloc(); pd_phys = pmm_alloc(); }
        uint64_t *pdpt = P2V(pdpt_phys), *pd = P2V(pd_phys);
        pd[0] = 0 | PTE_P | PTE_W | PTE_PS;
        pdpt[0] = pd_phys | PTE_P | PTE_W;
        kernel_pml4[0] = pdpt_phys | PTE_P | PTE_W;
    } else {
        kernel_pml4[0] = 0;
    }
    write_cr3(read_cr3());
}

/* ------------------------------------------------------------------------
 * vmalloc: virtually contiguous, physically scattered
 * ---------------------------------------------------------------------- */

void *vmalloc_pages(size_t pages)
{
    uint64_t va = __atomic_fetch_add(&vmalloc_next, (pages + 1) * PAGE_SIZE, __ATOMIC_RELAXED);
    if (va + pages * PAGE_SIZE > VMALLOC_END) return NULL;
    for (size_t i = 0; i < pages; i++) {
        uint64_t pa = pmm_alloc();
        if (!pa) {
            for (size_t j = 0; j < i; j++) vmm_unmap(&kernel_space, va + j * PAGE_SIZE, true);
            return NULL;
        }
        vmm_map(&kernel_space, va + i * PAGE_SIZE, pa, PTE_W | PTE_G | PTE_NX);
    }
    return (void *)va;
}

void vfree_pages(void *p, size_t pages)
{
    uint64_t va = (uint64_t)p;
    for (size_t i = 0; i < pages; i++)
        vmm_unmap(&kernel_space, va + i * PAGE_SIZE, true);
}

/* ------------------------------------------------------------------------
 * init
 * ---------------------------------------------------------------------- */

static void map_kernel_range(char *start, char *end, uint64_t flags)
{
    for (uint64_t va = ALIGN_DOWN((uint64_t)start, PAGE_SIZE); va < (uint64_t)end; va += PAGE_SIZE) {
        uint64_t *pte = walk(&kernel_space, va, true);
        if (!pte) panic("vmm: out of memory mapping kernel");
        *pte = (va - KERNEL_VBASE + kva_to_phys_base) | flags | PTE_P | PTE_G;
    }
}

void vmm_init(struct bootinfo *bi)
{
    kva_to_phys_base = bi->kernel_phys;
    hhdm_size = bi->hhdm_size;
    kernel_space.pml4_phys = kvirt_to_phys(kernel_pml4);
    if (kernel_space.pml4_phys >= 0x100000000ull) panic("vmm: kernel above 4 GiB");

    uint64_t nx = cpu_info.nx ? PTE_NX : 0;

    /* Pre-create every kernel-half PDPT so all address spaces share them. */
    for (int i = 256; i < 512; i++) {
        uint64_t pa = pmm_alloc();
        if (!pa) panic("vmm: out of memory");
        kernel_pml4[i] = pa | PTE_P | PTE_W;
    }

    /* Direct map with 2 MiB pages. */
    for (uint64_t pa = 0; pa < hhdm_size; pa += 0x200000) {
        uint64_t va = HHDM_BASE + pa;
        uint64_t *pdpt = P2V(kernel_pml4[(va >> 39) & 511] & PTE_ADDR);
        uint64_t *pd = table_next(pdpt, (va >> 30) & 511, true, false);
        if (!pd) panic("vmm: out of memory");
        pd[(va >> 21) & 511] = pa | PTE_P | PTE_W | PTE_PS | PTE_G | nx;
    }

    /* Kernel image with proper permissions. */
    map_kernel_range(__text_start, __text_end, 0);
    map_kernel_range(__rodata_start, __rodata_end, nx);
    map_kernel_range(__data_start, __kernel_end, PTE_W | nx);

    write_cr3(kernel_space.pml4_phys);

    /* Framebuffer: write-combining makes blits several times faster. */
    vmm_set_cache(bi->fb.phys, (uint64_t)bi->fb.pitch * bi->fb.height, PTE_WC);

    klog("vmm: direct map %lu MiB, kernel %lu KiB at phys %#lx",
         hhdm_size >> 20, bi->kernel_size >> 10, bi->kernel_phys);
}

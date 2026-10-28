/*
 * Physical memory manager: one bit per 4 KiB page.
 */
#include <mm.h>
#include <spinlock.h>

static uint64_t *bitmap;          /* 1 = used */
static uint64_t  npages;          /* pages covered by the bitmap */
static uint64_t  total_usable;
static uint64_t  free_count;
static uint64_t  next_hint;
static spinlock_t pmm_lock = SPINLOCK_INIT("pmm");
static struct bootinfo *boot;
static uint16_t *shares;          /* per page: references beyond the first (pages mapped by several address spaces) */

static inline void set_used(uint64_t p) { bitmap[p >> 6] |= 1ull << (p & 63); }
static inline void set_free(uint64_t p) { bitmap[p >> 6] &= ~(1ull << (p & 63)); }
static inline bool is_used(uint64_t p) { return bitmap[p >> 6] & (1ull << (p & 63)); }

void pmm_init(struct bootinfo *bi)
{
    boot = bi;
    uint64_t top = 0;
    for (uint32_t i = 0; i < bi->mmap_count; i++) {
        struct boot_mmap_entry *e = &bi->mmap[i];
        if (e->type == MEM_USABLE || e->type == MEM_ACPI_RECL || e->type == MEM_BOOT) {
            uint64_t end = e->base + e->length;
            if (end > top) top = end;
        }
    }
    npages = top / PAGE_SIZE;
    uint64_t bitmap_bytes = ALIGN_UP((npages + 63) / 64 * 8, PAGE_SIZE);

    /* Put the bitmap in the first usable region big enough, above 1 MiB. */
    uint64_t bm_phys = 0;
    for (uint32_t i = 0; i < bi->mmap_count; i++) {
        struct boot_mmap_entry *e = &bi->mmap[i];
        if (e->type != MEM_USABLE) continue;
        uint64_t base = ALIGN_UP(MAX(e->base, 0x100000ull), PAGE_SIZE);
        uint64_t end = ALIGN_DOWN(e->base + e->length, PAGE_SIZE);
        if (end > base && end - base >= bitmap_bytes) { bm_phys = base; break; }
    }
    if (!bm_phys) panic("pmm: no room for the page bitmap");
    bitmap = P2V(bm_phys);
    memset(bitmap, 0xFF, bitmap_bytes);

    for (uint32_t i = 0; i < bi->mmap_count; i++) {
        struct boot_mmap_entry *e = &bi->mmap[i];
        if (e->type != MEM_USABLE) continue;
        uint64_t first = ALIGN_UP(e->base, PAGE_SIZE) / PAGE_SIZE;
        uint64_t last = ALIGN_DOWN(e->base + e->length, PAGE_SIZE) / PAGE_SIZE;
        for (uint64_t p = first; p < last && p < npages; p++) {
            set_free(p);
            free_count++;
        }
    }
    /* Keep the low 1 MiB (real-mode stuff, SMP trampoline) and the bitmap. */
    for (uint64_t p = 0; p < 256 && p < npages; p++)
        if (!is_used(p)) { set_used(p); free_count--; }
    for (uint64_t p = bm_phys / PAGE_SIZE; p < (bm_phys + bitmap_bytes) / PAGE_SIZE; p++)
        if (!is_used(p)) { set_used(p); free_count--; }

    total_usable = free_count;
    next_hint = 256;
    /* the share counts: two bytes per page, taken from the same pool */
    size_t share_pages = (size_t)((npages * 2 + PAGE_SIZE - 1) / PAGE_SIZE);
    uint64_t sp = pmm_alloc_contig(share_pages);
    if (sp) shares = P2V(sp);
    klog("pmm: %lu MiB usable, %lu MiB free, bitmap %lu KiB",
         (total_usable * PAGE_SIZE) >> 20, (free_count * PAGE_SIZE) >> 20, bitmap_bytes >> 10);
}

/* ACPI reclaimable memory becomes normal RAM once the tables are parsed. */
void pmm_reclaim_acpi(void)
{
    spin_lock(&pmm_lock);
    uint64_t n = 0;
    for (uint32_t i = 0; i < boot->mmap_count; i++) {
        struct boot_mmap_entry *e = &boot->mmap[i];
        if (e->type != MEM_ACPI_RECL) continue;
        uint64_t first = ALIGN_UP(e->base, PAGE_SIZE) / PAGE_SIZE;
        uint64_t last = ALIGN_DOWN(e->base + e->length, PAGE_SIZE) / PAGE_SIZE;
        for (uint64_t p = MAX(first, 256ull); p < last && p < npages; p++)
            if (is_used(p)) { set_free(p); free_count++; total_usable++; n++; }
    }
    spin_unlock(&pmm_lock);
    if (n) klog("pmm: reclaimed %lu KiB of ACPI memory", (n * PAGE_SIZE) >> 10);
}

static uint64_t find_free_run(size_t count)
{
    uint64_t start = next_hint, run = 0, p = start;
    for (uint64_t scanned = 0; scanned < npages; scanned++, p++) {
        if (p >= npages) { p = 256; run = 0; }
        if (count == 1 && (p & 63) == 0 && bitmap[p >> 6] == ~0ull) {
            /* skip full words quickly */
            p += 63;
            scanned += 63;
            run = 0;
            continue;
        }
        if (is_used(p)) { run = 0; continue; }
        if (++run == count) return p - count + 1;
    }
    return 0;
}

uint64_t pmm_alloc_contig(size_t count)
{
    spin_lock(&pmm_lock);
    uint64_t first = find_free_run(count);
    if (!first) {
        spin_unlock(&pmm_lock);
        return 0;
    }
    for (uint64_t p = first; p < first + count; p++) set_used(p);
    free_count -= count;
    next_hint = first + count;
    spin_unlock(&pmm_lock);
    uint64_t pa = first * PAGE_SIZE;
    memset(P2V(pa), 0, count * PAGE_SIZE);
    return pa;
}

/* `count` zeroed contiguous pages that end below `limit` */
uint64_t pmm_alloc_contig_below(size_t count, uint64_t limit)
{
    spin_lock(&pmm_lock);
    uint64_t end = MIN(npages, limit / PAGE_SIZE), run = 0;
    for (uint64_t p = 256; p < end; p++) {
        if (is_used(p)) { run = 0; continue; }
        if (++run == count) {
            uint64_t first = p - count + 1;
            for (uint64_t q = first; q <= p; q++) set_used(q);
            free_count -= count;
            spin_unlock(&pmm_lock);
            memset(P2V(first * PAGE_SIZE), 0, count * PAGE_SIZE);
            return first * PAGE_SIZE;
        }
    }
    spin_unlock(&pmm_lock);
    return 0;
}

/* one zeroed page below `limit` (for DMA engines limited to 32-bit addresses) */
uint64_t pmm_alloc_below(uint64_t limit)
{
    spin_lock(&pmm_lock);
    uint64_t end = MIN(npages, limit / PAGE_SIZE);
    for (uint64_t p = 256; p < end; p++) {
        if (!is_used(p)) {
            set_used(p);
            free_count--;
            spin_unlock(&pmm_lock);
            memset(P2V(p * PAGE_SIZE), 0, PAGE_SIZE);
            return p * PAGE_SIZE;
        }
    }
    spin_unlock(&pmm_lock);
    return 0;
}

uint64_t pmm_alloc(void)
{
    return pmm_alloc_contig(1);
}

void pmm_free_contig(uint64_t pa, size_t count)
{
    uint64_t first = pa / PAGE_SIZE;
    spin_lock(&pmm_lock);
    for (uint64_t p = first; p < first + count; p++) {
        if (p < 256 || p >= npages || !is_used(p))
            panic("pmm: bad free of %#lx", p * PAGE_SIZE);
        set_free(p);
        free_count++;
    }
    if (first < next_hint) next_hint = first;
    spin_unlock(&pmm_lock);
}

void pmm_free(uint64_t pa)
{
    pmm_free_contig(pa, 1);
}

/* A page mapped by more than one address space (fork, shared memory) carries one extra count per additional user;
 * pmm_put drops one reference and frees the page when the last one is gone. */
void pmm_share(uint64_t pa)
{
    if (shares) __atomic_add_fetch(&shares[pa / PAGE_SIZE], 1, __ATOMIC_ACQ_REL);
}

unsigned pmm_shared(uint64_t pa)
{
    return shares ? __atomic_load_n(&shares[pa / PAGE_SIZE], __ATOMIC_ACQUIRE) : 0;
}

void pmm_put(uint64_t pa)
{
    if (shares) {
        uint16_t *c = &shares[pa / PAGE_SIZE];
        uint16_t v = __atomic_load_n(c, __ATOMIC_ACQUIRE);
        while (v) {
            if (__atomic_compare_exchange_n(c, &v, (uint16_t)(v - 1), false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
        }
    }
    pmm_free(pa);
}

uint64_t pmm_total_pages(void) { return total_usable; }
uint64_t pmm_free_pages(void) { return free_count; }

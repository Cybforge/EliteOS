/*
 * Kernel heap: segregated free lists for small blocks, vmalloc for big ones.
 * Every block carries a 16-byte header so payloads stay 16-byte aligned.
 */
#include <mm.h>
#include <spinlock.h>

#define MAGIC_USED 0x5A48454Du   /* "ZHEM" */
#define MAGIC_FREE 0x46524545u   /* "FREE" */
#define CLASS_BIG  0xFF

struct hdr {
    uint32_t magic;
    uint32_t cls;
    uint64_t size;          /* requested size, or page count for big blocks */
};

struct free_block {
    struct hdr h;
    struct free_block *next;
};

static const uint32_t class_size[] = {
    32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048, 3072, 4096,
};
#define NCLASSES ARRAY_SIZE(class_size)
#define CHUNK_PAGES 4

static struct free_block *free_lists[NCLASSES];
static spinlock_t heap_lock = SPINLOCK_INIT("heap");
static size_t bytes_used;

void heap_init(void) {}

static int class_for(size_t total)
{
    for (unsigned i = 0; i < NCLASSES; i++)
        if (total <= class_size[i]) return (int)i;
    return -1;
}

static bool refill(int cls)
{
    uint64_t pa = pmm_alloc_contig(CHUNK_PAGES);
    if (!pa) return false;
    uint8_t *base = P2V(pa);
    size_t bs = class_size[cls];
    size_t n = (CHUNK_PAGES * PAGE_SIZE) / bs;
    for (size_t i = 0; i < n; i++) {
        struct free_block *b = (struct free_block *)(base + i * bs);
        b->h.magic = MAGIC_FREE;
        b->h.cls = (uint32_t)cls;
        b->next = free_lists[cls];
        free_lists[cls] = b;
    }
    return true;
}

void *kmalloc(size_t size)
{
    if (!size) size = 1;
    size_t total = size + sizeof(struct hdr);
    int cls = class_for(total);
    if (cls < 0) {
        size_t pages = ALIGN_UP(total, PAGE_SIZE) / PAGE_SIZE;
        struct hdr *h = vmalloc_pages(pages);
        if (!h) return NULL;
        h->magic = MAGIC_USED;
        h->cls = CLASS_BIG;
        h->size = pages;
        __atomic_add_fetch(&bytes_used, pages * PAGE_SIZE, __ATOMIC_RELAXED);
        return h + 1;
    }
    spin_lock(&heap_lock);
    if (!free_lists[cls] && !refill(cls)) {
        spin_unlock(&heap_lock);
        return NULL;
    }
    struct free_block *b = free_lists[cls];
    free_lists[cls] = b->next;
    bytes_used += class_size[cls];
    spin_unlock(&heap_lock);
    if (b->h.magic != MAGIC_FREE) panic("heap: corrupted free list (class %d)", cls);
    b->h.magic = MAGIC_USED;
    b->h.size = size;
    return &b->h + 1;
}

void kfree(void *p)
{
    if (!p) return;
    struct hdr *h = (struct hdr *)p - 1;
    if (h->magic == MAGIC_FREE) panic("heap: double free of %p", p);
    if (h->magic != MAGIC_USED) panic("heap: bad free of %p (magic %#x)", p, h->magic);
    if (h->cls == CLASS_BIG) {
        size_t pages = h->size;
        h->magic = MAGIC_FREE;
        __atomic_sub_fetch(&bytes_used, pages * PAGE_SIZE, __ATOMIC_RELAXED);
        vfree_pages(h, pages);
        return;
    }
    struct free_block *b = (struct free_block *)h;
    b->h.magic = MAGIC_FREE;
    spin_lock(&heap_lock);
    b->next = free_lists[b->h.cls];
    free_lists[b->h.cls] = b;
    bytes_used -= class_size[b->h.cls];
    spin_unlock(&heap_lock);
}

void *kzalloc(size_t size)
{
    void *p = kmalloc(size);
    if (p) memset(p, 0, size);
    return p;
}

void *kcalloc(size_t n, size_t size)
{
    return kzalloc(n * size);
}

void *krealloc(void *p, size_t size)
{
    if (!p) return kmalloc(size);
    struct hdr *h = (struct hdr *)p - 1;
    size_t old = h->cls == CLASS_BIG ? h->size * PAGE_SIZE - sizeof(struct hdr)
                                     : class_size[h->cls] - sizeof(struct hdr);
    if (size <= old) {
        if (h->cls != CLASS_BIG) h->size = size;
        return p;
    }
    void *n = kmalloc(size);
    if (!n) return NULL;
    memcpy(n, p, old);
    kfree(p);
    return n;
}

size_t heap_used(void) { return bytes_used; }

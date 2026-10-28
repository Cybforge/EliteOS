/*
 * User memory for programs of the System V x86-64 interface: areas (VMAs),
 * pages made on first touch, copy-on-write after fork, shared memory objects,
 * safe copies between kernel and user space.
 */
#include <kernel.h>
#include <cpu.h>
#include <x86.h>
#include <sched.h>
#include <dev.h>
#include <umm.h>
#include <abierr.h>

#define USER_LO 0x10000ull                         /* nothing is mapped below this (null pointer guard) */

/* ------------------------------------------------------------------------
 * areas
 * ---------------------------------------------------------------------- */

static struct vma *find_vma(struct mm *m, uint64_t addr)
{
    struct vma *h = m->hint;
    if (h && addr >= h->start && addr < h->end) return h;
    for (struct vma *v = m->vmas; v; v = v->next) {
        if (addr < v->start) return NULL;
        if (addr < v->end) { m->hint = v; return v; }
    }
    return NULL;
}

static void insert_vma(struct mm *m, struct vma *v)
{
    struct vma **pp = &m->vmas;
    while (*pp && (*pp)->start < v->start) pp = &(*pp)->next;
    v->next = *pp;
    *pp = v;
    m->vm_pages += (v->end - v->start) / PAGE_SIZE;
}

static void unlink_vma(struct mm *m, struct vma *v)
{
    for (struct vma **pp = &m->vmas; *pp; pp = &(*pp)->next)
        if (*pp == v) { *pp = v->next; break; }
    if (m->hint == v) m->hint = NULL;
    m->vm_pages -= (v->end - v->start) / PAGE_SIZE;
}

static void free_vma(struct vma *v)
{
    if (v->file) vfs_close(v->file);
    if (v->shm) shm_put(v->shm);
    kfree(v);
}

/* cut `v` in two at `at`; the new area is [at, end) */
static struct vma *split_vma(struct mm *m, struct vma *v, uint64_t at)
{
    struct vma *n = kzalloc(sizeof(*n));
    if (!n) return NULL;
    *n = *v;
    n->start = at;
    if (n->file) { vfs_ref(n->file); n->foff += at - v->start; }
    if (n->shm) shm_get(n->shm);
    v->end = at;
    n->next = v->next;
    v->next = n;
    m->hint = NULL;
    return n;
}

static uint64_t pte_flags_for(uint32_t prot, uint32_t vflags, bool cow)
{
    uint64_t f = PTE_U;
    if ((prot & VM_WRITE) && !cow) f |= PTE_W;
    if (!(prot & VM_EXEC) && cpu_info.nx) f |= PTE_NX;
    (void)vflags;
    return f;
}

/* ------------------------------------------------------------------------
 * shared memory objects
 * ---------------------------------------------------------------------- */

struct shm *shm_new(uint64_t size, const char *name)
{
    struct shm *s = kzalloc(sizeof(*s));
    if (!s) return NULL;
    s->refs = 1;
    s->size = size;
    s->npages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (s->npages) {
        s->pages = kzalloc(s->npages * sizeof(uint64_t));
        if (!s->pages) { kfree(s); return NULL; }
    }
    if (name) strlcpy(s->name, name, sizeof(s->name));
    return s;
}

struct shm *shm_get(struct shm *s)
{
    if (s) __atomic_add_fetch(&s->refs, 1, __ATOMIC_ACQ_REL);
    return s;
}

void shm_put(struct shm *s)
{
    if (!s || __atomic_sub_fetch(&s->refs, 1, __ATOMIC_ACQ_REL) > 0) return;
    for (uint64_t i = 0; i < s->npages; i++)
        if (s->pages[i]) pmm_put(s->pages[i]);
    kfree(s->pages);
    kfree(s);
}

uint64_t shm_page(struct shm *s, uint64_t idx)
{
    if (idx >= s->npages) return 0;
    mutex_lock(&s->lock);
    if (!s->pages[idx]) s->pages[idx] = pmm_alloc();
    uint64_t pa = s->pages[idx];
    mutex_unlock(&s->lock);
    return pa;
}

int shm_resize(struct shm *s, uint64_t size)
{
    uint64_t np = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    mutex_lock(&s->lock);
    if (np > s->npages) {
        uint64_t *n = kzalloc(np * sizeof(uint64_t));
        if (!n) { mutex_unlock(&s->lock); return -ERR_NOMEM; }
        memcpy(n, s->pages, s->npages * sizeof(uint64_t));
        kfree(s->pages);
        s->pages = n;
        s->npages = np;
    } else {
        for (uint64_t i = np; i < s->npages; i++)
            if (s->pages[i]) { pmm_put(s->pages[i]); s->pages[i] = 0; }
        /* the tail of the last page must read as zero if the object grows again */
        if (size & 0xFFF && np && s->pages[np - 1]) memset((uint8_t *)P2V(s->pages[np - 1]) + (size & 0xFFF), 0, PAGE_SIZE - (size & 0xFFF));
    }
    s->size = size;
    mutex_unlock(&s->lock);
    return 0;
}

long shm_read(struct shm *s, uint64_t off, void *buf, size_t n)
{
    if (off >= s->size) return 0;
    n = (size_t)MIN((uint64_t)n, s->size - off);
    size_t done = 0;
    while (done < n) {
        uint64_t pa = shm_page(s, (off + done) / PAGE_SIZE);
        if (!pa) return done ? (long)done : -ERR_NOMEM;
        size_t c = MIN(n - done, PAGE_SIZE - ((off + done) & 0xFFF));
        memcpy((uint8_t *)buf + done, (uint8_t *)P2V(pa) + ((off + done) & 0xFFF), c);
        done += c;
    }
    return (long)done;
}

long shm_write(struct shm *s, uint64_t off, const void *buf, size_t n)
{
    if (off + n > s->size && shm_resize(s, off + n)) return -ERR_NOSPC;
    size_t done = 0;
    while (done < n) {
        uint64_t pa = shm_page(s, (off + done) / PAGE_SIZE);
        if (!pa) return done ? (long)done : -ERR_NOMEM;
        size_t c = MIN(n - done, PAGE_SIZE - ((off + done) & 0xFFF));
        memcpy((uint8_t *)P2V(pa) + ((off + done) & 0xFFF), (const uint8_t *)buf + done, c);
        done += c;
    }
    return (long)done;
}

/* ------------------------------------------------------------------------
 * address spaces
 * ---------------------------------------------------------------------- */

struct mm *mm_new(void)
{
    struct mm *m = kzalloc(sizeof(*m));
    if (!m) return NULL;
    m->as = vmm_create_space();
    if (!m->as) { kfree(m); return NULL; }
    m->lock = (mutex_t)MUTEX_INIT("mm");
    m->refs = 1;
    m->stack_top = UMM_STACK_TOP;
    m->mmap_base = UMM_MMAP_BASE;
    m->rlimit_stack = UMM_STACK_MAX;
    m->rlimit_as = ~0ull;
    m->rlimit_data = ~0ull;
    return m;
}

struct mm *mm_get(struct mm *m)
{
    if (m) __atomic_add_fetch(&m->refs, 1, __ATOMIC_ACQ_REL);
    return m;
}

/* drop every page of [start, end) from the page tables (mm->lock held). The frames are only given back once no CPU
 * can still reach them through a stale translation. */
static void zap_range(struct mm *m, uint64_t start, uint64_t end)
{
    uint64_t frames[512];
    for (uint64_t chunk = start; chunk < end;) {
        uint64_t stop = MIN(end, chunk + 512 * PAGE_SIZE);
        int n = 0;
        for (uint64_t va = chunk; va < stop; va += PAGE_SIZE) {
            uint64_t *pte = vmm_pte(m->as, va, false);
            if (!pte || !(*pte & PTE_P)) continue;
            frames[n++] = *pte & PTE_ADDR;
            *pte = 0;
            m->rss_pages--;
        }
        if (n) {
            tlb_shootdown(m, chunk, stop);
            for (int i = 0; i < n; i++) pmm_put(frames[i]);
        }
        chunk = stop;
    }
}

void mm_put(struct mm *m)
{
    if (!m || __atomic_sub_fetch(&m->refs, 1, __ATOMIC_ACQ_REL) > 0) return;
    while (m->vmas) {
        struct vma *v = m->vmas;
        zap_range(m, v->start, v->end);
        m->vmas = v->next;
        free_vma(v);
    }
    vmm_destroy_space(m->as);
    kfree(m);
}

/* a copy for fork: private pages become shared copy-on-write, shared memory stays shared */
struct mm *mm_fork(struct mm *old)
{
    struct mm *m = mm_new();
    if (!m) return NULL;
    mutex_lock(&old->lock);
    m->brk_start = old->brk_start;
    m->brk = old->brk;
    m->mmap_base = old->mmap_base;
    m->stack_top = old->stack_top;
    m->rlimit_stack = old->rlimit_stack;
    m->rlimit_as = old->rlimit_as;
    m->rlimit_data = old->rlimit_data;
    struct vma **tail = &m->vmas;
    bool ok = true;
    for (struct vma *v = old->vmas; v && ok; v = v->next) {
        struct vma *n = kzalloc(sizeof(*n));
        if (!n) { ok = false; break; }
        *n = *v;
        n->next = NULL;
        if (n->file) vfs_ref(n->file);
        if (n->shm) shm_get(n->shm);
        *tail = n;
        tail = &n->next;
        m->vm_pages += (v->end - v->start) / PAGE_SIZE;
        bool shared = v->flags & VM_SHARED;
        for (uint64_t va = v->start; va < v->end; va += PAGE_SIZE) {
            uint64_t *pte = vmm_pte(old->as, va, false);
            if (!pte || !(*pte & PTE_P)) continue;
            uint64_t pa = *pte & PTE_ADDR;
            uint64_t fl = *pte & ~PTE_ADDR;
            if (!shared && (fl & (PTE_W | PTE_COW))) {
                fl = (fl & ~PTE_W) | PTE_COW;                     /* both sides write-protect it; the first write copies */
                *pte = pa | fl;
            }
            pmm_share(pa);
            if (!vmm_map(m->as, va, pa, fl)) { pmm_put(pa); ok = false; break; }
            m->rss_pages++;
        }
    }
    tlb_shootdown(old, USER_LO, UMM_STACK_TOP);
    mutex_unlock(&old->lock);
    if (!ok) { mm_put(m); return NULL; }
    return m;
}

/* ------------------------------------------------------------------------
 * mmap, munmap, mprotect, brk
 * ---------------------------------------------------------------------- */

static bool range_ok(uint64_t addr, uint64_t len)
{
    return addr >= USER_LO && addr + len > addr && addr + len <= UMM_STACK_TOP;
}

/* the highest free gap of `len` bytes below `hi` (mm->lock held), 0 if none */
static uint64_t find_gap(struct mm *m, uint64_t len, uint64_t hi)
{
    uint64_t best = 0, prev_end = USER_LO;
    for (struct vma *v = m->vmas;; v = v->next) {
        uint64_t gap_end = v ? MIN(v->start, hi) : hi;
        if (gap_end > prev_end && gap_end - prev_end >= len) best = gap_end - len;
        if (!v || v->start >= hi) break;
        prev_end = MAX(prev_end, v->end);
    }
    return best;
}

static bool overlaps(struct mm *m, uint64_t start, uint64_t end)
{
    for (struct vma *v = m->vmas; v; v = v->next) {
        if (v->start >= end) break;
        if (v->end > start) return true;
    }
    return false;
}

long mm_munmap_locked(struct mm *m, uint64_t addr, uint64_t len)
{
    if (addr & 0xFFF || !len) return -ERR_INVAL;
    len = ALIGN_UP(len, PAGE_SIZE);
    uint64_t end = addr + len;
    if (!range_ok(addr, len)) return -ERR_INVAL;
    for (struct vma *v = m->vmas; v;) {
        if (v->end <= addr) { v = v->next; continue; }
        if (v->start >= end) break;
        if (v->start < addr) {                             /* the first part stays: carry on with the rest */
            struct vma *tail = split_vma(m, v, addr);
            if (!tail) return -ERR_NOMEM;
            v = tail;
            continue;
        }
        if (v->end > end && !split_vma(m, v, end)) return -ERR_NOMEM;     /* the last part stays */
        struct vma *next = v->next;
        zap_range(m, v->start, v->end);
        unlink_vma(m, v);
        free_vma(v);
        v = next;
    }
    tlb_shootdown(m, addr, end);
    return 0;
}

long mm_munmap(struct mm *m, uint64_t addr, uint64_t len)
{
    mutex_lock(&m->lock);
    long r = mm_munmap_locked(m, addr, len);
    mutex_unlock(&m->lock);
    return r;
}

long mm_mmap(struct mm *m, uint64_t addr, uint64_t len, uint32_t prot, uint32_t flags, bool fixed, bool noreplace,
             vnode_t *file, uint64_t off, struct shm *shm, const char *name)
{
    if (!len) return -ERR_INVAL;
    len = ALIGN_UP(len, PAGE_SIZE);
    if (len > (1ull << 46)) return -ERR_NOMEM;
    mutex_lock(&m->lock);
    long r;
    if ((m->vm_pages + len / PAGE_SIZE) * PAGE_SIZE > m->rlimit_as) { r = -ERR_NOMEM; goto out; }
    if (fixed) {
        if (addr & 0xFFF || !range_ok(addr, len)) { r = -ERR_INVAL; goto out; }
        if (noreplace && overlaps(m, addr, addr + len)) { r = -ERR_EXIST; goto out; }
        r = mm_munmap_locked(m, addr, len);
        if (r) goto out;
    } else {
        uint64_t a = 0;
        if (addr && !(addr & 0xFFF) && range_ok(addr, len) && !overlaps(m, addr, addr + len)) a = addr;     /* a hint that fits */
        if (!a) a = find_gap(m, len, m->mmap_base);
        if (!a) { r = -ERR_NOMEM; goto out; }
        addr = a;
    }
    /* grow the area before this one if it is the same kind of memory (keeps the list short) */
    struct vma *prev = NULL;
    for (struct vma *v = m->vmas; v && v->end <= addr; v = v->next) prev = v;
    if (prev && prev->end == addr && !file && !shm && !prev->file && !prev->shm && prev->prot == prot && prev->flags == flags &&
        !name && !prev->name[0] && !(flags & (VM_STACK | VM_GROWSDOWN))) {
        prev->end += len;
        m->vm_pages += len / PAGE_SIZE;
        r = (long)addr;
        goto out;
    }
    struct vma *v = kzalloc(sizeof(*v));
    if (!v) { r = -ERR_NOMEM; goto out; }
    v->start = addr;
    v->end = addr + len;
    v->prot = prot;
    v->flags = flags;
    v->file = file ? vfs_ref(file) : NULL;
    v->foff = off;
    v->shm = shm_get(shm);
    if (name) strlcpy(v->name, name, sizeof(v->name));
    insert_vma(m, v);
    r = (long)addr;
out:
    mutex_unlock(&m->lock);
    return r;
}

static void protect_pages(struct mm *m, struct vma *v)
{
    for (uint64_t va = v->start; va < v->end; va += PAGE_SIZE) {
        uint64_t *pte = vmm_pte(m->as, va, false);
        if (!pte || !(*pte & PTE_P)) continue;
        /* a page that other address spaces map too must not become writable for one of them: it is copied on write */
        bool cow = (*pte & PTE_COW) || (!(v->flags & VM_SHARED) && pmm_shared(*pte & PTE_ADDR));
        uint64_t keep = *pte & ~(PTE_W | PTE_NX | PTE_COW);
        *pte = keep | (pte_flags_for(v->prot, v->flags, cow) & (PTE_W | PTE_NX)) | (cow ? PTE_COW : 0);
    }
}

long mm_mprotect(struct mm *m, uint64_t addr, uint64_t len, uint32_t prot)
{
    if (addr & 0xFFF) return -ERR_INVAL;
    len = ALIGN_UP(len, PAGE_SIZE);
    if (!len) return 0;
    if (!range_ok(addr, len)) return -ERR_NOMEM;
    uint64_t end = addr + len;
    mutex_lock(&m->lock);
    /* every page of the range has to be mapped */
    uint64_t at = addr;
    for (struct vma *v = m->vmas; v && at < end; v = v->next) {
        if (v->end <= at) continue;
        if (v->start > at) break;
        at = v->end;
    }
    if (at < end) { mutex_unlock(&m->lock); return -ERR_NOMEM; }
    for (struct vma *v = m->vmas; v;) {
        struct vma *next = v->next;
        if (v->end <= addr) { v = next; continue; }
        if (v->start >= end) break;
        if (v->start < addr) { struct vma *t = split_vma(m, v, addr); if (!t) { mutex_unlock(&m->lock); return -ERR_NOMEM; } v = t; next = v->next; }
        if (v->end > end) { if (!split_vma(m, v, end)) { mutex_unlock(&m->lock); return -ERR_NOMEM; } next = v->next; }
        v->prot = (v->prot & ~(uint32_t)7) | prot;
        protect_pages(m, v);
        v = next;
    }
    tlb_shootdown(m, addr, end);
    mutex_unlock(&m->lock);
    return 0;
}

long mm_brk(struct mm *m, uint64_t newbrk)
{
    mutex_lock(&m->lock);
    uint64_t cur = m->brk;
    if (!newbrk || newbrk < m->brk_start) { mutex_unlock(&m->lock); return (long)cur; }
    uint64_t want = ALIGN_UP(newbrk, PAGE_SIZE), have = ALIGN_UP(cur, PAGE_SIZE);
    if (want > have) {
        if (want > m->brk_start + m->rlimit_data || overlaps(m, have, want) || want >= m->mmap_base - (1ull << 30)) {
            mutex_unlock(&m->lock);
            return (long)cur;                                  /* refused: the old break */
        }
        struct vma *heap = NULL;
        for (struct vma *v = m->vmas; v; v = v->next) if (v->end == have && !strcmp(v->name, "[heap]")) heap = v;
        if (heap) {
            heap->end = want;
            m->vm_pages += (want - have) / PAGE_SIZE;
        } else {
            struct vma *v = kzalloc(sizeof(*v));
            if (!v) { mutex_unlock(&m->lock); return (long)cur; }
            v->start = have;
            v->end = want;
            v->prot = VM_READ | VM_WRITE;
            strlcpy(v->name, "[heap]", sizeof(v->name));
            insert_vma(m, v);
        }
    } else if (want < have) {
        mm_munmap_locked(m, want, have - want);
    }
    m->brk = newbrk;
    mutex_unlock(&m->lock);
    return (long)newbrk;
}

long mm_madvise(struct mm *m, uint64_t addr, uint64_t len, int advice)
{
    if (addr & 0xFFF) return -ERR_INVAL;
    len = ALIGN_UP(len, PAGE_SIZE);
    if (!range_ok(addr, len)) return -ERR_NOMEM;
    if (advice == 4 || advice == 8) {                          /* DONTNEED, FREE: the pages are given back */
        mutex_lock(&m->lock);
        for (struct vma *v = m->vmas; v; v = v->next) {
            if (v->end <= addr) continue;
            if (v->start >= addr + len) break;
            if (v->flags & VM_SHARED) continue;
            zap_range(m, MAX(v->start, addr), MIN(v->end, addr + len));
        }
        tlb_shootdown(m, addr, addr + len);
        mutex_unlock(&m->lock);
    }
    return 0;
}

long mm_msync(struct mm *m, uint64_t addr, uint64_t len)
{
    UNUSED(m); UNUSED(addr); UNUSED(len);
    return 0;
}

long mm_mremap(struct mm *m, uint64_t old, uint64_t oldlen, uint64_t newlen, int mflags, uint64_t newaddr)
{
    if (old & 0xFFF || !newlen) return -ERR_INVAL;
    oldlen = ALIGN_UP(oldlen, PAGE_SIZE);
    newlen = ALIGN_UP(newlen, PAGE_SIZE);
    mutex_lock(&m->lock);
    struct vma *v = find_vma(m, old);
    long r;
    if (!v || old + oldlen > v->end) { r = -ERR_FAULT; goto out; }
    if (newlen == oldlen) { r = (long)old; goto out; }
    if (newlen < oldlen) {
        r = mm_munmap_locked(m, old + newlen, oldlen - newlen);
        if (!r) r = (long)old;
        goto out;
    }
    /* growing: in place if the space after it is free */
    if (old + oldlen == v->end && !overlaps(m, v->end, old + newlen) && range_ok(old, newlen)) {
        m->vm_pages += (old + newlen - v->end) / PAGE_SIZE;
        v->end = old + newlen;
        r = (long)old;
        goto out;
    }
    if (!(mflags & 1)) { r = -ERR_NOMEM; goto out; }           /* MREMAP_MAYMOVE not given */
    {
        uint64_t dst = (mflags & 2) ? newaddr : find_gap(m, newlen, m->mmap_base);    /* MREMAP_FIXED */
        if (!dst) { r = -ERR_NOMEM; goto out; }
        if (mflags & 2) mm_munmap_locked(m, dst, newlen);
        struct vma *n = kzalloc(sizeof(*n));
        if (!n) { r = -ERR_NOMEM; goto out; }
        *n = *v;
        n->start = dst;
        n->end = dst + newlen;
        n->foff = v->foff + (old - v->start);
        if (n->file) vfs_ref(n->file);
        if (n->shm) shm_get(n->shm);
        n->next = NULL;
        insert_vma(m, n);
        /* move the pages that are present */
        for (uint64_t off = 0; off < oldlen; off += PAGE_SIZE) {
            uint64_t *pte = vmm_pte(m->as, old + off, false);
            if (!pte || !(*pte & PTE_P)) continue;
            uint64_t val = *pte;
            *pte = 0;
            vmm_map(m->as, dst + off, val & PTE_ADDR, val & ~PTE_ADDR);
        }
        tlb_shootdown(m, old, old + oldlen);
        /* drop the old area (its pages were moved, so nothing is released twice) */
        struct vma *ov = find_vma(m, old);
        if (ov && ov != n) {
            if (ov->start < old) { struct vma *t = split_vma(m, ov, old); if (t) ov = t; }
            if (ov->end > old + oldlen) split_vma(m, ov, old + oldlen);
            unlink_vma(m, ov);
            free_vma(ov);
        }
        r = (long)dst;
    }
out:
    mutex_unlock(&m->lock);
    return r;
}

size_t mm_maps(struct mm *m, char *buf, size_t cap)
{
    size_t o = 0;
    mutex_lock(&m->lock);
    for (struct vma *v = m->vmas; v && o + 128 < cap; v = v->next) {
        o += (size_t)snprintf(buf + o, cap - o, "%016lx-%016lx %c%c%c%c %08lx 00:00 0 %s\n", v->start, v->end,
                              v->prot & VM_READ ? 'r' : '-', v->prot & VM_WRITE ? 'w' : '-', v->prot & VM_EXEC ? 'x' : '-',
                              v->flags & VM_SHARED ? 's' : 'p', v->foff, v->name);
    }
    mutex_unlock(&m->lock);
    return o;
}

/* ------------------------------------------------------------------------
 * page faults
 * ---------------------------------------------------------------------- */

/* mm->lock held. 0 if the page is usable for the access afterwards, else a negative errno */
static int fault_locked(struct mm *m, uint64_t va, bool write, bool exec)
{
    va = ALIGN_DOWN(va, PAGE_SIZE);
    struct vma *v = find_vma(m, va);
    if (!v) {
        /* the stack grows down on demand */
        struct vma *s = NULL;
        for (struct vma *x = m->vmas; x; x = x->next)
            if (x->start > va) { s = x; break; }
        if (s && (s->flags & VM_GROWSDOWN) && va >= m->stack_top - m->rlimit_stack && s->start - va < (1ull << 22)) {
            uint64_t nstart = va;
            if (overlaps(m, nstart, s->start)) return -ERR_FAULT;
            m->vm_pages += (s->start - nstart) / PAGE_SIZE;
            s->start = nstart;
            v = s;
        } else {
            return -ERR_FAULT;
        }
    }
    if (write && !(v->prot & VM_WRITE)) return -ERR_FAULT;
    if (exec && !(v->prot & VM_EXEC)) return -ERR_FAULT;
    if (!write && !exec && !(v->prot & (VM_READ | VM_EXEC | VM_WRITE))) return -ERR_FAULT;

    uint64_t *pte = vmm_pte(m->as, va, false);
    if (pte && (*pte & PTE_P)) {
        if (!write || (*pte & PTE_W)) return 0;                 /* another thread got here first */
        if (!(*pte & PTE_COW)) {
            *pte |= PTE_W;                                      /* the area is writable: the entry was only slow to update */
            return 0;
        }
        /* copy on write */
        uint64_t old = *pte & PTE_ADDR;
        if (!pmm_shared(old)) {                                 /* nobody else has it: just allow the write */
            *pte = (*pte | PTE_W) & ~PTE_COW;
            invlpg(va);
            return 0;
        }
        uint64_t np = pmm_alloc();
        if (!np) return -ERR_NOMEM;
        memcpy(P2V(np), P2V(old), PAGE_SIZE);
        uint64_t fl = (*pte & ~PTE_ADDR & ~PTE_COW) | PTE_W;
        *pte = np | fl;
        invlpg(va);
        tlb_shootdown(m, va, va + PAGE_SIZE);
        pmm_put(old);                                           /* only now: no CPU may still write through the old entry */
        return 0;
    }

    uint64_t pa;
    uint64_t fl = pte_flags_for(v->prot, v->flags, false);
    if (v->shm) {
        pa = shm_page(v->shm, (v->foff + (va - v->start)) / PAGE_SIZE);
        if (!pa) return -ERR_FAULT;                             /* beyond the end of the object */
        pmm_share(pa);                                          /* the object keeps its own reference */
    } else {
        pa = pmm_alloc();
        if (!pa) return -ERR_NOMEM;
        if (v->file) {
            uint64_t foff = v->foff + (va - v->start);
            long got = vfs_read(v->file, foff, P2V(pa), PAGE_SIZE);   /* the rest of the page stays zero */
            if (got < 0) { pmm_put(pa); return -ERR_IO; }
        }
    }
    if (!vmm_map(m->as, va, pa, fl)) { pmm_put(pa); return -ERR_NOMEM; }
    m->rss_pages++;
    return 0;
}

/* what the memory map says about an address that could not be handled (for the log) */
void mm_fault_report(struct mm *m, uint64_t addr, uint64_t error)
{
    mutex_lock(&m->lock);
    struct vma *v = find_vma(m, addr);
    uint64_t *pte = vmm_pte(m->as, addr, false);
    klog("umm: unhandled fault at %#lx (error %#lx): %s [%#lx-%#lx) prot %u flags %#x, entry %#lx", addr, error,
         v ? "area" : "no area", v ? v->start : 0, v ? v->end : 0, v ? v->prot : 0, v ? v->flags : 0, pte ? *pte : 0);
    mutex_unlock(&m->lock);
}

bool mm_fault(struct mm *m, uint64_t addr, uint64_t error)
{
    if (addr < USER_LO || addr >= UMM_STACK_TOP + PAGE_SIZE) return false;
    if (error & 8) return false;                                /* a reserved bit: a damaged entry, not a missing page */
    bool write = error & 2, exec = error & 16;
    mutex_lock(&m->lock);
    int r = fault_locked(m, addr, write, exec);
    mutex_unlock(&m->lock);
    return r == 0;
}

/* ------------------------------------------------------------------------
 * copies between kernel and user memory
 * ---------------------------------------------------------------------- */

struct mm *mm_current(void)
{
    thread_t *t = thread_current();
    return t->proc ? t->proc->mm : NULL;
}

bool user_range_ok(uint64_t addr, size_t n)
{
    return addr >= USER_LO && addr + n >= addr && addr + n <= UMM_STACK_TOP + PAGE_SIZE;
}

/* the physical address behind a user address, making the page present (and private, for a write) first */
static uint64_t user_phys(struct mm *m, uint64_t va, bool write)
{
    for (int tries = 0; tries < 3; tries++) {
        uint64_t *pte = vmm_pte(m->as, va, false);
        if (pte && (*pte & PTE_P) && (!write || (*pte & PTE_W))) return (*pte & PTE_ADDR) + (va & 0xFFF);
        if (fault_locked(m, va, write, false)) return 0;
    }
    return 0;
}

/* copy between kernel memory and the memory of `m` (which does not have to be the running program's) */
int mm_access(struct mm *m, uint64_t addr, void *kbuf, size_t n, bool write)
{
    if (!m || !user_range_ok(addr, n)) return -ERR_FAULT;
    uint8_t *k = kbuf;
    while (n) {
        size_t c = MIN(n, (size_t)(PAGE_SIZE - (addr & 0xFFF)));
        mutex_lock(&m->lock);
        uint64_t pa = user_phys(m, addr, write);
        if (!pa) { mutex_unlock(&m->lock); return -ERR_FAULT; }
        if (write) memcpy(P2V(pa), k, c);
        else memcpy(k, P2V(pa), c);
        mutex_unlock(&m->lock);
        addr += c;
        k += c;
        n -= c;
    }
    return 0;
}

/* is the memory at va shared with other address spaces (shared memory, MAP_SHARED)? */
bool mm_is_shared(struct mm *m, uint64_t va)
{
    mutex_lock(&m->lock);
    struct vma *v = find_vma(m, va);
    bool s = v && (v->flags & VM_SHARED);
    mutex_unlock(&m->lock);
    return s;
}

/* the physical page behind va right now, without making it present or taking the lock (0 if there is none) */
uint64_t mm_peek_phys(struct mm *m, uint64_t va)
{
    uint64_t *pte = vmm_pte(m->as, va, false);
    return pte && (*pte & PTE_P) ? (*pte & PTE_ADDR) + (va & 0xFFF) : 0;
}

/* the physical address of a user word (the key of a futex), making the page present */
uint64_t mm_user_phys(struct mm *m, uint64_t va, bool write)
{
    if (!user_range_ok(va, 1)) return 0;
    mutex_lock(&m->lock);
    uint64_t pa = user_phys(m, va, write);
    mutex_unlock(&m->lock);
    return pa;
}

int copy_from_user(void *dst, uint64_t src, size_t n) { return mm_access(mm_current(), src, dst, n, false); }
int copy_to_user(uint64_t dst, const void *src, size_t n) { return mm_access(mm_current(), dst, (void *)src, n, true); }

int clear_user(uint64_t dst, size_t n)
{
    uint8_t zero[256];
    memset(zero, 0, sizeof(zero));
    while (n) {
        size_t c = MIN(n, sizeof(zero));
        int r = mm_access(mm_current(), dst, zero, c, true);
        if (r) return r;
        dst += c;
        n -= c;
    }
    return 0;
}

long strncpy_from_user(char *dst, uint64_t src, size_t max)
{
    struct mm *m = mm_current();
    if (!m) return -ERR_FAULT;
    size_t len = 0;
    while (len < max) {
        if (!user_range_ok(src + len, 1)) return -ERR_FAULT;
        size_t c = MIN(max - len, (size_t)(PAGE_SIZE - ((src + len) & 0xFFF)));
        mutex_lock(&m->lock);
        uint64_t pa = user_phys(m, src + len, false);
        if (!pa) { mutex_unlock(&m->lock); return -ERR_FAULT; }
        const char *p = P2V(pa);
        size_t i;
        for (i = 0; i < c; i++) {
            dst[len + i] = p[i];
            if (!p[i]) { mutex_unlock(&m->lock); return (long)(len + i); }
        }
        mutex_unlock(&m->lock);
        len += c;
    }
    return -ERR_NAMETOOLONG;
}

long copy_string_array(uint64_t uarr, char ***out)
{
    *out = NULL;
    if (!uarr) { *out = kzalloc(sizeof(char *)); return 0; }
    long n = 0;
    char **arr = kzalloc(sizeof(char *) * 1024);
    if (!arr) return -ERR_NOMEM;
    for (;;) {
        uint64_t p;
        if (n >= 1023) { free_string_array(arr, n); return -ERR_2BIG; }
        if (copy_from_user(&p, uarr + (uint64_t)n * 8, 8)) { free_string_array(arr, n); return -ERR_FAULT; }
        if (!p) break;
        char *s = kmalloc(4096);
        if (!s) { free_string_array(arr, n); return -ERR_NOMEM; }
        long l = strncpy_from_user(s, p, 4096);
        if (l < 0) { kfree(s); free_string_array(arr, n); return l == -ERR_NAMETOOLONG ? -ERR_2BIG : l; }
        arr[n++] = s;
    }
    *out = arr;
    return n;
}

void free_string_array(char **a, long n)
{
    if (!a) return;
    for (long i = 0; i < n; i++) kfree(a[i]);
    kfree(a);
}

/* ------------------------------------------------------------------------
 * TLB shootdown
 * ---------------------------------------------------------------------- */

#define VEC_IPI_TLB 0xF2

static struct {
    mutex_t lock;
    addrspace_t *as;
    uint64_t start, end;
    volatile int pending;
} tlb = { .lock = MUTEX_INIT("tlb") };

static void flush_range(uint64_t start, uint64_t end)
{
    if ((end - start) / PAGE_SIZE > 64) { write_cr3(read_cr3()); return; }
    for (uint64_t va = start; va < end; va += PAGE_SIZE) invlpg(va);
}

static void tlb_ipi(struct regs *r, void *ctx)
{
    UNUSED(r); UNUSED(ctx);
    if (this_cpu()->active_as == tlb.as) flush_range(tlb.start, tlb.end);
    __atomic_sub_fetch(&tlb.pending, 1, __ATOMIC_ACQ_REL);
}

void tlb_shootdown(struct mm *m, uint64_t start, uint64_t end)
{
    flush_range(start, end);
    if (ncpus <= 1) return;
    mutex_lock(&tlb.lock);
    tlb.as = m->as;
    tlb.start = start;
    tlb.end = end;
    struct cpu *me = this_cpu();
    int sent = 0;
    for (int i = 0; i < ncpus; i++) {
        struct cpu *c = &cpus[i];
        if (c == me || !c->online || c->active_as != m->as) continue;
        __atomic_add_fetch(&tlb.pending, 1, __ATOMIC_ACQ_REL);
        lapic_send_ipi(c->lapic_id, VEC_IPI_TLB);
        sent++;
    }
    uint64_t t0 = uptime_ms();
    while (sent && __atomic_load_n(&tlb.pending, __ATOMIC_ACQUIRE) > 0) {
        __asm__ volatile("pause");
        if (uptime_ms() - t0 > 200) { klog("umm: TLB shootdown timed out"); tlb.pending = 0; break; }
    }
    mutex_unlock(&tlb.lock);
}

void umm_init(void)
{
    irq_register(VEC_IPI_TLB, tlb_ipi, NULL);
}

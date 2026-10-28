#ifndef ZENITH_UMM_H
#define ZENITH_UMM_H

/*
 * Memory of user programs that use the System V x86-64 interface: an address
 * space is a list of areas (VMAs); pages are made when they are first touched,
 * private pages are copied on the first write after a fork, and shared memory
 * objects can be mapped by several programs at once.
 */
#include <kernel.h>
#include <mm.h>
#include <vfs.h>

#define VM_READ   1
#define VM_WRITE  2
#define VM_EXEC   4
#define VM_SHARED 0x100          /* changes are seen by every mapper (shared memory) */
#define VM_GROWSDOWN 0x200       /* the stack */
#define VM_STACK  0x400
#define VM_LOCKED 0x800

#define UMM_STACK_TOP   0x00007FFFFFFFF000ull
#define UMM_MMAP_BASE   0x00007FFFF0000000ull     /* mappings are placed downwards from here */
#define UMM_STACK_MAX   (8ull << 20)              /* how far the stack may grow */

/* a block of memory that can be mapped by several address spaces (memfd, SysV shm, Wayland buffers) */
struct shm {
    int refs;
    uint64_t size;
    uint64_t npages;
    uint64_t *pages;             /* physical addresses, 0 = not allocated yet */
    mutex_t lock;
    char name[32];
};

struct vma {
    uint64_t start, end;
    uint32_t prot;               /* VM_READ | VM_WRITE | VM_EXEC */
    uint32_t flags;
    vnode_t *file;               /* mapped file, or NULL */
    uint64_t foff;               /* offset in the file of `start` */
    struct shm *shm;
    char name[32];               /* "[heap]", "[stack]", ... for /proc/self/maps */
    struct vma *next;
};

struct mm {
    addrspace_t *as;
    mutex_t lock;
    struct vma *vmas;            /* sorted by address */
    struct vma *hint;
    uint64_t brk_start, brk;
    uint64_t mmap_base;
    uint64_t stack_top;
    int refs;
    uint64_t rss_pages;          /* pages present */
    uint64_t vm_pages;           /* pages mapped */
    uint64_t rlimit_stack;
    uint64_t rlimit_as;
    uint64_t rlimit_data;
};

struct mm *mm_new(void);
struct mm *mm_fork(struct mm *old);
struct mm *mm_get(struct mm *m);
void       mm_put(struct mm *m);

/* all return a negative errno on failure */
long mm_mmap(struct mm *m, uint64_t addr, uint64_t len, uint32_t prot, uint32_t flags, bool fixed, bool noreplace,
             vnode_t *file, uint64_t off, struct shm *shm, const char *name);
long mm_munmap(struct mm *m, uint64_t addr, uint64_t len);
long mm_mprotect(struct mm *m, uint64_t addr, uint64_t len, uint32_t prot);
long mm_brk(struct mm *m, uint64_t newbrk);
long mm_mremap(struct mm *m, uint64_t old, uint64_t oldlen, uint64_t newlen, int mflags, uint64_t newaddr);
long mm_madvise(struct mm *m, uint64_t addr, uint64_t len, int advice);
long mm_msync(struct mm *m, uint64_t addr, uint64_t len);
size_t mm_maps(struct mm *m, char *buf, size_t cap);     /* /proc/<pid>/maps */

/* a page fault from user mode: true if it is fixed and the instruction can run again */
void mm_fault_report(struct mm *m, uint64_t addr, uint64_t error);
bool mm_fault(struct mm *m, uint64_t addr, uint64_t error);

/* the current program's memory */
struct mm *mm_current(void);
int  mm_access(struct mm *m, uint64_t addr, void *kbuf, size_t n, bool write);   /* any address space */
int  copy_from_user(void *dst, uint64_t src, size_t n);
int  copy_to_user(uint64_t dst, const void *src, size_t n);
long strncpy_from_user(char *dst, uint64_t src, size_t max);   /* length without the NUL, or a negative errno */
int  clear_user(uint64_t dst, size_t n);
bool user_range_ok(uint64_t addr, size_t n);
uint64_t mm_user_phys(struct mm *m, uint64_t va, bool write);
uint64_t mm_peek_phys(struct mm *m, uint64_t va);
bool mm_is_shared(struct mm *m, uint64_t va);
/* a user string array (argv, envp): returns the count or a negative errno; strings are kmalloc'd */
long copy_string_array(uint64_t uarr, char ***out);
void free_string_array(char **a, long n);

/* shared memory objects */
struct shm *shm_new(uint64_t size, const char *name);
struct shm *shm_get(struct shm *s);
void        shm_put(struct shm *s);
int         shm_resize(struct shm *s, uint64_t size);
long        shm_read(struct shm *s, uint64_t off, void *buf, size_t n);
long        shm_write(struct shm *s, uint64_t off, const void *buf, size_t n);
uint64_t    shm_page(struct shm *s, uint64_t index);            /* physical address, allocated on demand */

/* flush stale translations on every CPU that is running this address space */
void tlb_shootdown(struct mm *m, uint64_t start, uint64_t end);
void umm_init(void);

#endif

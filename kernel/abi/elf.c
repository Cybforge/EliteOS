/*
 * The program loader: maps ELF executables and shared objects (ET_EXEC and
 * ET_DYN, with or without an interpreter) into an address space and builds the
 * initial stack (arguments, environment, auxiliary vector).
 *
 * Segments are mapped straight from the file and loaded page by page when they
 * are touched, so a very large program starts as fast as a small one.
 */
#include <kernel.h>
#include <cpu.h>
#include <x86.h>
#include <umm.h>
#include <abierr.h>
#include <vfs.h>
#include <crypto.h>
#include "sv.h"

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} Ehdr;

typedef struct {
    uint32_t p_type, p_flags;
    uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
} Phdr;

#define PT_LOAD 1
#define PT_INTERP 3
#define PT_PHDR 6
#define PT_TLS 7
#define PT_GNU_STACK 0x6474e551
#define ET_EXEC 2
#define ET_DYN 3
#define PF_X 1
#define PF_W 2
#define PF_R 4

static uint32_t cpu_hwcap(void)
{
    uint32_t a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d);
    return d;
}

#define PIE_BASE 0x0000555555554000ull

static void errf(char *err, size_t n, const char *fmt, ...)
{
    if (!err || !n) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

/* read the headers of an ELF file; the program headers are kmalloc'd */
static int read_headers(vnode_t *vn, Ehdr *eh, Phdr **ph, char *err, size_t errn)
{
    if (vfs_read(vn, 0, eh, sizeof(*eh)) != (long)sizeof(*eh) || memcmp(eh->e_ident, "\x7F" "ELF", 4)) {
        errf(err, errn, "not an ELF file");
        return -ERR_NOEXEC;
    }
    if (eh->e_ident[4] != 2 || eh->e_ident[5] != 1 || eh->e_machine != 62 || (eh->e_type != ET_EXEC && eh->e_type != ET_DYN)) {
        errf(err, errn, "not an x86-64 executable");
        return -ERR_NOEXEC;
    }
    if (eh->e_phentsize != sizeof(Phdr) || eh->e_phnum == 0 || eh->e_phnum > 128) {
        errf(err, errn, "damaged program headers");
        return -ERR_NOEXEC;
    }
    size_t n = (size_t)eh->e_phnum * sizeof(Phdr);
    *ph = kmalloc(n);
    if (!*ph) return -ERR_NOMEM;
    if (vfs_read(vn, eh->e_phoff, *ph, n) != (long)n) {
        kfree(*ph);
        errf(err, errn, "the program headers are cut off");
        return -ERR_NOEXEC;
    }
    return 0;
}

static uint32_t prot_of(uint32_t pf)
{
    return (pf & PF_R ? VM_READ : 0) | (pf & PF_W ? VM_WRITE : 0) | (pf & PF_X ? VM_EXEC : 0);
}

/* map every PT_LOAD of one file at `bias`; reports the lowest and highest address, the entry and where the headers are */
static int map_segments(struct mm *m, vnode_t *vn, const char *path, const Ehdr *eh, const Phdr *ph, uint64_t bias,
                        uint64_t *lo_out, uint64_t *hi_out, uint64_t *phdr_addr, char *err, size_t errn)
{
    uint64_t lo = ~0ull, hi = 0;
    *phdr_addr = 0;
    bool first = true;
    for (int i = 0; i < eh->e_phnum; i++) {
        const Phdr *p = &ph[i];
        if (p->p_type != PT_LOAD || !p->p_memsz) continue;
        if (p->p_filesz > p->p_memsz || p->p_offset % PAGE_SIZE != p->p_vaddr % PAGE_SIZE) {
            errf(err, errn, "a segment is not aligned like its file position");
            return -ERR_NOEXEC;
        }
        uint64_t start = ALIGN_DOWN(bias + p->p_vaddr, PAGE_SIZE);
        uint64_t file_end = bias + p->p_vaddr + p->p_filesz;
        uint64_t mem_end = ALIGN_UP(bias + p->p_vaddr + p->p_memsz, PAGE_SIZE);
        uint32_t prot = prot_of(p->p_flags);
        uint64_t off = ALIGN_DOWN(p->p_offset, PAGE_SIZE);
        uint64_t file_pages_end = ALIGN_UP(file_end, PAGE_SIZE);
        if (first) {
            /* the program headers are in the first segment's file range */
            if (eh->e_phoff >= p->p_offset && eh->e_phoff + (uint64_t)eh->e_phnum * sizeof(Phdr) <= p->p_offset + p->p_filesz)
                *phdr_addr = bias + p->p_vaddr + (eh->e_phoff - p->p_offset);
            first = false;
        }
        lo = MIN(lo, start);
        hi = MAX(hi, mem_end);
        if (file_pages_end > start) {
            long r = mm_mmap(m, start, file_pages_end - start, prot, 0, true, false, vn, off, NULL, NULL);
            if (r < 0) { errf(err, errn, "cannot map %s", path); return (int)r; }
        }
        if (mem_end > file_pages_end) {                      /* the zero-filled rest (bss) */
            long r = mm_mmap(m, file_pages_end, mem_end - file_pages_end, prot, 0, true, false, NULL, 0, NULL, NULL);
            if (r < 0) { errf(err, errn, "cannot map the bss of %s", path); return (int)r; }
        }
        /* the part of the last file page behind the file data has to read as zero */
        if (p->p_memsz > p->p_filesz && (file_end & 0xFFF) && (prot & VM_WRITE)) {
            uint8_t zero[PAGE_SIZE];
            memset(zero, 0, sizeof(zero));
            size_t n = (size_t)(PAGE_SIZE - (file_end & 0xFFF));
            if (mm_access(m, file_end, zero, n, true)) {
                errf(err, errn, "cannot clear the end of a segment of %s", path);
                return -ERR_FAULT;
            }
        }
    }
    if (lo == ~0ull) { errf(err, errn, "%s has nothing to load", path); return -ERR_NOEXEC; }
    *lo_out = lo;
    *hi_out = hi;
    return 0;
}

/* the span of addresses an object needs (for choosing a bias) */
static void span_of(const Ehdr *eh, const Phdr *ph, uint64_t *lo, uint64_t *hi)
{
    *lo = ~0ull;
    *hi = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD || !ph[i].p_memsz) continue;
        *lo = MIN(*lo, ALIGN_DOWN(ph[i].p_vaddr, PAGE_SIZE));
        *hi = MAX(*hi, ALIGN_UP(ph[i].p_vaddr + ph[i].p_memsz, PAGE_SIZE));
    }
}

int sv_load_elf(struct mm *m, struct sfile *unused, const char *path, struct sv_elf_info *out, char *err, size_t errn)
{
    UNUSED(unused);
    memset(out, 0, sizeof(*out));
    vnode_t *vn = vfs_open(path, false);
    if (!vn || vn->type != VN_FILE) {
        if (vn) vfs_close(vn);
        errf(err, errn, "cannot open %s", path);
        return vn ? -ERR_ACCES : -ERR_NOENT;
    }
    Ehdr eh;
    Phdr *ph = NULL;
    int r = read_headers(vn, &eh, &ph, err, errn);
    if (r) { vfs_close(vn); return r; }

    uint64_t bias = 0;
    if (eh.e_type == ET_DYN) {
        uint64_t lo, hi;
        span_of(&eh, ph, &lo, &hi);
        bias = PIE_BASE - lo;
    } else {
        for (int i = 0; i < eh.e_phnum; i++)
            if (ph[i].p_type == PT_LOAD && ph[i].p_vaddr < 0x10000) { errf(err, errn, "%s wants to be loaded at address 0", path); r = -ERR_NOEXEC; goto done; }
    }
    uint64_t lo, hi, phdr_addr;
    r = map_segments(m, vn, path, &eh, ph, bias, &lo, &hi, &phdr_addr, err, errn);
    if (r) goto done;
    out->base = bias;
    out->top = hi;
    out->prog_entry = bias + eh.e_entry;
    out->entry = out->prog_entry;
    out->phnum = eh.e_phnum;
    out->phent = sizeof(Phdr);
    out->phdr = phdr_addr;
    for (int i = 0; i < eh.e_phnum; i++) {
        if (ph[i].p_type == PT_PHDR) out->phdr = bias + ph[i].p_vaddr;
        if (ph[i].p_type == PT_INTERP && ph[i].p_filesz > 1 && ph[i].p_filesz < sizeof(out->interp)) {
            if (vfs_read(vn, ph[i].p_offset, out->interp, ph[i].p_filesz) != (long)ph[i].p_filesz) { r = -ERR_NOEXEC; errf(err, errn, "damaged interpreter name"); goto done; }
            out->interp[ph[i].p_filesz] = 0;
            out->has_interp = true;
        }
    }
    if (!out->phdr) { errf(err, errn, "the program headers of %s are not mapped", path); r = -ERR_NOEXEC; goto done; }

    if (out->has_interp) {
        vnode_t *iv = vfs_open(out->interp, false);
        if (!iv || iv->type != VN_FILE) {
            if (iv) vfs_close(iv);
            errf(err, errn, "the program interpreter %s is missing", out->interp);
            r = -ERR_NOENT;
            goto done;
        }
        Ehdr ie;
        Phdr *iph = NULL;
        r = read_headers(iv, &ie, &iph, err, errn);
        if (!r) {
            uint64_t ilo, ihi, ibias;
            span_of(&ie, iph, &ilo, &ihi);
            if (ie.e_type == ET_DYN) {
                /* reserve a free stretch below the mmap area, then map the segments over it */
                long a = mm_mmap(m, 0, ihi - ilo, 0, 0, false, false, NULL, 0, NULL, NULL);
                if (a < 0) { r = (int)a; errf(err, errn, "no room for the interpreter"); }
                else {
                    mm_munmap(m, (uint64_t)a, ihi - ilo);
                    ibias = (uint64_t)a - ilo;
                    uint64_t l2, h2, pa2;
                    r = map_segments(m, iv, out->interp, &ie, iph, ibias, &l2, &h2, &pa2, err, errn);
                    if (!r) {
                        out->interp_base = ibias;
                        out->entry = ibias + ie.e_entry;
                    }
                }
            } else {
                uint64_t l2, h2, pa2;
                r = map_segments(m, iv, out->interp, &ie, iph, 0, &l2, &h2, &pa2, err, errn);
                if (!r) { out->interp_base = 0; out->entry = ie.e_entry; }
            }
            kfree(iph);
        }
        vfs_close(iv);
    }
done:
    kfree(ph);
    vfs_close(vn);
    return r;
}

/* ------------------------------------------------------------------------
 * the initial stack
 * ---------------------------------------------------------------------- */

enum {
    AT_NULL = 0, AT_PHDR = 3, AT_PHENT = 4, AT_PHNUM = 5, AT_PAGESZ = 6, AT_BASE = 7, AT_FLAGS = 8, AT_ENTRY = 9,
    AT_UID = 11, AT_EUID = 12, AT_GID = 13, AT_EGID = 14, AT_PLATFORM = 15, AT_HWCAP = 16, AT_CLKTCK = 17,
    AT_SECURE = 23, AT_RANDOM = 25, AT_HWCAP2 = 26, AT_EXECFN = 31, AT_MINSIGSTKSZ = 51,
};

/* Build the stack of a new program image at the top of m's address space; returns the stack pointer (argc) or 0 */
uint64_t sv_build_stack(struct mm *m, const struct sv_elf_info *info, char **argv, int argc, char **envp, int envc,
                        const char *execfn, uint32_t uid, uint32_t gid, uint64_t *arg_start, uint64_t *arg_end,
                        uint64_t *env_start, uint64_t *env_end, char *err, size_t errn)
{
    uint64_t top = m->stack_top;
    /* the stack area; it grows down from here on demand */
    long r = mm_mmap(m, top - 128 * 1024, 128 * 1024, VM_READ | VM_WRITE, VM_GROWSDOWN | VM_STACK, true, false, NULL, 0, NULL, "[stack]");
    if (r < 0) { errf(err, errn, "no room for the stack"); return 0; }

    uint64_t sp = top - 16;
    /* strings first, from the top down */
    uint64_t execfn_p, platform_p, random_p;
    size_t l = strlen(execfn) + 1;
    sp -= l;
    execfn_p = sp;
    if (mm_access(m, sp, (void *)execfn, l, true)) goto fault;
    uint64_t *envp_p = kmalloc((size_t)(envc + 1) * 8), *argv_p = kmalloc((size_t)(argc + 1) * 8);
    if (!envp_p || !argv_p) { kfree(envp_p); kfree(argv_p); return 0; }
    uint64_t env_hi = sp;
    for (int i = envc - 1; i >= 0; i--) {
        l = strlen(envp[i]) + 1;
        sp -= l;
        envp_p[i] = sp;
        if (mm_access(m, sp, envp[i], l, true)) { kfree(envp_p); kfree(argv_p); goto fault; }
    }
    *env_start = sp;
    *env_end = env_hi;
    uint64_t arg_hi = sp;
    for (int i = argc - 1; i >= 0; i--) {
        l = strlen(argv[i]) + 1;
        sp -= l;
        argv_p[i] = sp;
        if (mm_access(m, sp, argv[i], l, true)) { kfree(envp_p); kfree(argv_p); goto fault; }
    }
    *arg_start = sp;
    *arg_end = arg_hi;
    static const char platform[] = "x86_64";
    sp -= sizeof(platform);
    platform_p = sp;
    mm_access(m, sp, (void *)platform, sizeof(platform), true);
    uint8_t rnd[16];
    random_bytes(rnd, sizeof(rnd));
    sp -= 16;
    random_p = sp;
    mm_access(m, sp, rnd, 16, true);
    sp = ALIGN_DOWN(sp, 16);

    uint64_t aux[40];
    int na = 0;
#define AUX(t, v) do { aux[na++] = (t); aux[na++] = (uint64_t)(v); } while (0)
    AUX(AT_PHDR, info->phdr);
    AUX(AT_PHENT, info->phent);
    AUX(AT_PHNUM, info->phnum);
    AUX(AT_PAGESZ, PAGE_SIZE);
    AUX(AT_BASE, info->interp_base);
    AUX(AT_FLAGS, 0);
    AUX(AT_ENTRY, info->prog_entry);
    AUX(AT_UID, uid);
    AUX(AT_EUID, uid);
    AUX(AT_GID, gid);
    AUX(AT_EGID, gid);
    AUX(AT_PLATFORM, platform_p);
    AUX(AT_HWCAP, cpu_hwcap());
    AUX(AT_CLKTCK, 100);
    AUX(AT_SECURE, 0);
    AUX(AT_RANDOM, random_p);
    AUX(AT_HWCAP2, 0);
    AUX(AT_EXECFN, execfn_p);
    AUX(AT_MINSIGSTKSZ, 3072);
    AUX(AT_NULL, 0);
#undef AUX
    /* argc, argv[], NULL, envp[], NULL, auxv[] */
    size_t words = 1 + (size_t)argc + 1 + (size_t)envc + 1 + (size_t)na;
    sp -= words * 8;
    sp = ALIGN_DOWN(sp, 16);                                     /* argc sits on a 16-byte boundary */
    uint64_t *blk = kmalloc(words * 8);
    if (!blk) { kfree(envp_p); kfree(argv_p); return 0; }
    size_t k = 0;
    blk[k++] = (uint64_t)argc;
    for (int i = 0; i < argc; i++) blk[k++] = argv_p[i];
    blk[k++] = 0;
    for (int i = 0; i < envc; i++) blk[k++] = envp_p[i];
    blk[k++] = 0;
    for (int i = 0; i < na; i++) blk[k++] = aux[i];
    int rc = mm_access(m, sp, blk, words * 8, true);
    kfree(blk);
    kfree(envp_p);
    kfree(argv_p);
    if (rc) goto fault;
    m->stack_top = top;
    return sp;
fault:
    errf(err, errn, "the arguments do not fit on the stack");
    return 0;
}

/*
 * The module loader: maps a relocatable ELF object into the kernel's
 * module window (within 2 GiB of the kernel, so its calls and absolute
 * addresses reach kernel symbols), applies the x86-64 relocations against
 * the kernel's exported functions and runs its init function.
 */
#include <kernel.h>
#include <mm.h>
#include <module.h>
#include <spinlock.h>
#include <vfs.h>

#define MOD_BASE 0xFFFFFFFFA0000000ull
#define MOD_END  0xFFFFFFFFC0000000ull

struct elf_hdr {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} PACKED;

struct elf_sec {
    uint32_t name, type;
    uint64_t flags, addr, offset, size;
    uint32_t link, info;
    uint64_t addralign, entsize;
} PACKED;

struct elf_sym {
    uint32_t name;
    uint8_t info, other;
    uint16_t shndx;
    uint64_t value, size;
} PACKED;

struct elf_rela {
    uint64_t offset, info;
    int64_t addend;
} PACKED;

#define SHT_SYMTAB 2
#define SHT_RELA 4
#define SHT_NOBITS 8
#define SHF_ALLOC 2
#define SHN_UNDEF 0
#define SHN_ABS 0xFFF1
#define SHN_COMMON 0xFFF2
#define R_X86_64_64 1
#define R_X86_64_PC32 2
#define R_X86_64_PLT32 4
#define R_X86_64_32 10
#define R_X86_64_32S 11
#define R_X86_64_PC64 24

struct module {
    struct module *next;
    const struct zmodule *info;
    char origin[96];
    uint64_t base;
    size_t pages;
    bool initialised;
};

static struct module *modules;
static uint64_t next_va = MOD_BASE;
static mutex_t mod_lock_m = MUTEX_INIT("module");

/* map `pages` fresh pages at the next free address in the module window (RWX: modules are signed kernel code) */
static uint64_t window_alloc(size_t pages)
{
    uint64_t va = next_va;
    if (va + pages * PAGE_SIZE > MOD_END) return 0;
    for (size_t i = 0; i < pages; i++) {
        uint64_t pa = pmm_alloc();
        if (!pa || !vmm_map(&kernel_space, va + i * PAGE_SIZE, pa, PTE_W | PTE_G)) {
            for (size_t k = 0; k < i; k++) vmm_unmap(&kernel_space, va + k * PAGE_SIZE, true);
            return 0;
        }
    }
    next_va += (pages + 1) * PAGE_SIZE;                  /* one unmapped guard page between modules */
    return va;
}

static void errf(char *err, size_t n, const char *fmt, ...)
{
    if (!err || !n) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

int module_load(const void *image, size_t size, const char *origin, char *err, size_t errn)
{
    const uint8_t *img = image;
    const struct elf_hdr *eh = image;
    if (size < sizeof(*eh) || memcmp(eh->ident, "\x7f" "ELF", 4) || eh->ident[4] != 2 || eh->type != 1 || eh->machine != 62) {
        errf(err, errn, "not a relocatable x86-64 ELF object");
        return E_INVAL;
    }
    if (eh->shoff + (uint64_t)eh->shnum * sizeof(struct elf_sec) > size || eh->shentsize != sizeof(struct elf_sec)) {
        errf(err, errn, "damaged section table");
        return E_INVAL;
    }
    const struct elf_sec *sh = (const struct elf_sec *)(img + eh->shoff);
    int ns = eh->shnum;

    /* lay out the allocatable sections */
    uint64_t *addr = kzalloc((size_t)ns * sizeof(uint64_t));
    if (!addr) return E_NOMEM;
    size_t total = 0;
    for (int i = 0; i < ns; i++) {
        if (!(sh[i].flags & SHF_ALLOC)) continue;
        size_t al = sh[i].addralign ? (size_t)sh[i].addralign : 1;
        total = ALIGN_UP(total, al);
        addr[i] = total;                                  /* offset for now */
        total += sh[i].size;
        if (sh[i].type != SHT_NOBITS && sh[i].offset + sh[i].size > size) {
            errf(err, errn, "a section reaches past the end of the file");
            kfree(addr);
            return E_INVAL;
        }
    }
    size_t pages = ALIGN_UP(total ? total : 1, PAGE_SIZE) / PAGE_SIZE;
    mutex_lock(&mod_lock_m);
    uint64_t base = window_alloc(pages);
    if (!base) {
        mutex_unlock(&mod_lock_m);
        errf(err, errn, "no room in the module window");
        kfree(addr);
        return E_NOMEM;
    }
    memset((void *)base, 0, pages * PAGE_SIZE);
    for (int i = 0; i < ns; i++) {
        if (!(sh[i].flags & SHF_ALLOC)) continue;
        addr[i] += base;
        if (sh[i].type != SHT_NOBITS) memcpy((void *)addr[i], img + sh[i].offset, sh[i].size);
    }

    /* symbols */
    int rc = 0;
    const struct elf_sec *symtab = NULL;
    for (int i = 0; i < ns; i++) if (sh[i].type == SHT_SYMTAB) symtab = &sh[i];
    if (!symtab || symtab->link >= (uint32_t)ns) {
        errf(err, errn, "no symbol table");
        rc = E_INVAL;
        goto fail;
    }
    const struct elf_sym *syms = (const struct elf_sym *)(img + symtab->offset);
    size_t nsyms = symtab->size / sizeof(struct elf_sym);
    const char *strtab = (const char *)(img + sh[symtab->link].offset);
    uint64_t *symval = kzalloc(nsyms * sizeof(uint64_t));
    if (!symval) { rc = E_NOMEM; goto fail; }
    for (size_t i = 1; i < nsyms; i++) {
        const struct elf_sym *s = &syms[i];
        const char *name = strtab + s->name;
        if (s->shndx == SHN_ABS) symval[i] = s->value;
        else if (s->shndx == SHN_COMMON) { errf(err, errn, "common symbol %s (build with -fno-common)", name); rc = E_INVAL; break; }
        else if (s->shndx != SHN_UNDEF && s->shndx < ns) symval[i] = addr[s->shndx] + s->value;
        else if (s->shndx == SHN_UNDEF && name[0]) {
            uint64_t a = ksym_find(name);
            if (!a && (s->info >> 4) != 2) {                /* weak undefined symbols stay 0 */
                errf(err, errn, "undefined symbol %s", name);
                rc = E_INVAL;
                break;
            }
            symval[i] = a;
        }
    }
    if (rc) { kfree(symval); goto fail; }

    /* relocations */
    for (int i = 0; i < ns && !rc; i++) {
        if (sh[i].type != SHT_RELA || sh[i].info >= (uint32_t)ns || !(sh[sh[i].info].flags & SHF_ALLOC)) continue;
        const struct elf_rela *r = (const struct elf_rela *)(img + sh[i].offset);
        size_t nr = sh[i].size / sizeof(*r);
        uint64_t target = addr[sh[i].info];
        for (size_t k = 0; k < nr && !rc; k++) {
            uint32_t type = (uint32_t)r[k].info, si = (uint32_t)(r[k].info >> 32);
            if (si >= nsyms || r[k].offset + 8 > sh[sh[i].info].size + 8) { rc = E_INVAL; break; }
            uint64_t P = target + r[k].offset, S = symval[si];
            int64_t A = r[k].addend;
            switch (type) {
            case R_X86_64_64: *(uint64_t *)P = S + (uint64_t)A; break;
            case R_X86_64_PC64: *(uint64_t *)P = S + (uint64_t)A - P; break;
            case R_X86_64_PC32:
            case R_X86_64_PLT32: {
                int64_t v = (int64_t)(S + (uint64_t)A - P);
                if (v != (int32_t)v) { errf(err, errn, "relocation out of range (%s)", strtab + syms[si].name); rc = E_INVAL; break; }
                *(int32_t *)P = (int32_t)v;
                break;
            }
            case R_X86_64_32S: {
                int64_t v = (int64_t)(S + (uint64_t)A);
                if (v != (int32_t)v) { errf(err, errn, "absolute address does not fit (%s)", strtab + syms[si].name); rc = E_INVAL; break; }
                *(int32_t *)P = (int32_t)v;
                break;
            }
            case R_X86_64_32: {
                uint64_t v = S + (uint64_t)A;
                if (v != (uint32_t)v) { errf(err, errn, "absolute address does not fit (%s)", strtab + syms[si].name); rc = E_INVAL; break; }
                *(uint32_t *)P = (uint32_t)v;
                break;
            }
            default:
                errf(err, errn, "unsupported relocation type %u", type);
                rc = E_INVAL;
            }
        }
    }
    if (rc) { kfree(symval); goto fail; }

    /* find zmodule_info */
    const struct zmodule *info = NULL;
    for (size_t i = 1; i < nsyms; i++)
        if (!strcmp(strtab + syms[i].name, "zmodule_info") && syms[i].shndx != SHN_UNDEF) info = (const struct zmodule *)symval[i];
    kfree(symval);
    kfree(addr);
    if (!info || info->magic != ZMODULE_MAGIC) {
        errf(err, errn, "no zmodule_info");
        mutex_unlock(&mod_lock_m);
        return E_INVAL;
    }
    for (struct module *m = modules; m; m = m->next)
        if (!strcmp(m->info->name, info->name)) {
            errf(err, errn, "module %s is already loaded", info->name);
            mutex_unlock(&mod_lock_m);
            return E_EXIST;
        }
    struct module *m = kzalloc(sizeof(*m));
    m->info = info;
    m->base = base;
    m->pages = pages;
    strlcpy(m->origin, origin ? origin : "", sizeof(m->origin));
    m->next = modules;
    modules = m;
    mutex_unlock(&mod_lock_m);
    if (info->init) {
        int r = info->init();
        if (r) {
            errf(err, errn, "%s: init failed (%d)", info->name, r);
            mutex_lock(&mod_lock_m);
            for (struct module **pp = &modules; *pp; pp = &(*pp)->next)
                if (*pp == m) { *pp = m->next; break; }
            mutex_unlock(&mod_lock_m);
            kfree(m);
            return r < 0 ? r : E_IO;
        }
    }
    m->initialised = true;
    klog("module: %s %s loaded at %#lx (%lu KiB) from %s", info->name, info->version, base, (pages * PAGE_SIZE) >> 10,
         m->origin);
    return 0;

fail:
    mutex_unlock(&mod_lock_m);
    kfree(addr);
    return rc ? rc : E_IO;
}

int module_unload(const char *name)
{
    mutex_lock(&mod_lock_m);
    for (struct module **pp = &modules; *pp; pp = &(*pp)->next) {
        struct module *m = *pp;
        if (strcmp(m->info->name, name)) continue;
        *pp = m->next;
        mutex_unlock(&mod_lock_m);
        if (m->info->exit) m->info->exit();
        klog("module: %s unloaded", name);
        kfree(m);                                        /* the pages stay mapped: nothing may still point into them */
        return 0;
    }
    mutex_unlock(&mod_lock_m);
    return E_NOENT;
}

bool module_loaded(const char *name)
{
    for (struct module *m = modules; m; m = m->next)
        if (!strcmp(m->info->name, name)) return true;
    return false;
}

int module_list(struct module_status *out, int max)
{
    int n = 0;
    for (struct module *m = modules; m && n < max; m = m->next, n++) {
        strlcpy(out[n].name, m->info->name, sizeof(out[n].name));
        strlcpy(out[n].version, m->info->version, sizeof(out[n].version));
        strlcpy(out[n].description, m->info->description, sizeof(out[n].description));
        strlcpy(out[n].origin, m->origin, sizeof(out[n].origin));
        out[n].base = m->base;
        out[n].size = m->pages * PAGE_SIZE;
    }
    return n;
}

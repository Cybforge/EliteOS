/*
 * UEFI runtime services: reading and writing the firmware's variables, which is
 * how a boot entry is added and put first in the boot order.
 *
 * The loader leaves the firmware's runtime memory where it is (it does not call
 * SetVirtualAddressMap), so the services are called at their physical addresses:
 * a page table that maps that memory one-to-one, the kernel's own half on top,
 * is switched in for the duration of a call.
 */
#include <kernel.h>
#include <mm.h>
#include <x86.h>
#include <block.h>
#include <vfs.h>
#include <uefi.h>

extern struct bootinfo *boot_info;

#define EFI_RT_GetVariable 72
#define EFI_RT_SetVariable 88
#define EFI_RT_GetNextVariableName 80

typedef uint64_t (__attribute__((ms_abi)) *efi_fn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

static addrspace_t efi_as;
static uint64_t efi_low[256];                    /* the one-to-one half of the page table */
static uint64_t scratch_phys;
static bool ready, failed;
static mutex_t uefi_mtx = MUTEX_INIT("uefi");

#define SCRATCH_PAGES 16

static const uefi_guid_t global_guid = { 0x8BE4DF61, 0x93CA, 0x11D2, { 0xAA, 0x0D, 0x00, 0xE0, 0x98, 0x03, 0x2B, 0x8C } };

static bool setup(void)
{
    if (ready) return true;
    if (failed || !boot_info->efi_runtime || !boot_info->efi_rt_count) return false;
    failed = true;
    efi_as.pml4_phys = pmm_alloc();
    scratch_phys = pmm_alloc_contig(SCRATCH_PAGES);
    if (!efi_as.pml4_phys || !scratch_phys) return false;
    uint64_t pages = 0;
    for (uint32_t i = 0; i < boot_info->efi_rt_count; i++) {
        const struct boot_rt_region *r = &boot_info->efi_rt[i];
        uint64_t flags = PTE_W | (r->mmio ? PTE_UC : 0);
        for (uint64_t pa = r->phys; pa < r->phys + r->length; pa += PAGE_SIZE, pages++) {
            if (pages > 65536 || !vmm_map(&efi_as, pa, pa, flags)) return false;
        }
    }
    for (int i = 0; i < SCRATCH_PAGES; i++)
        if (!vmm_map(&efi_as, scratch_phys + (uint64_t)i * PAGE_SIZE, scratch_phys + (uint64_t)i * PAGE_SIZE, PTE_W | PTE_NX)) return false;
    /* the table itself is part of one of the runtime regions: check */
    bool in_region = false;
    for (uint32_t i = 0; i < boot_info->efi_rt_count; i++)
        if (boot_info->efi_runtime >= boot_info->efi_rt[i].phys &&
            boot_info->efi_runtime < boot_info->efi_rt[i].phys + boot_info->efi_rt[i].length) in_region = true;
    if (!in_region) return false;
    memcpy(efi_low, P2V(efi_as.pml4_phys), sizeof(efi_low));
    klog("uefi: runtime services at %#lx, %lu runtime pages mapped", boot_info->efi_runtime, pages);
    failed = false;
    ready = true;
    return true;
}

bool uefi_available(void) { return setup(); }

static uint64_t efi_call(uint32_t offset, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    uint64_t fn = *(uint64_t *)P2V(boot_info->efi_runtime + offset);    /* the table is in runtime memory, inside the direct map */
    uint64_t *pml4 = P2V(efi_as.pml4_phys);
    uint64_t rflags = read_rflags();
    cli();
    memcpy(pml4, efi_low, sizeof(efi_low));
    memcpy(pml4 + 256, P2V(kernel_space.pml4_phys) + 256 * 8, 256 * 8);   /* the kernel's half may have grown */
    uint64_t old = read_cr3();
    write_cr3(efi_as.pml4_phys);
    uint64_t r = ((efi_fn)fn)(a1, a2, a3, a4, a5, 0);
    write_cr3(old);
    if (rflags & (1 << 9)) sti();
    return r;
}

/* ---- variables ---- */

static void put_name(uint16_t *dst, const char *name)
{
    while (*name) *dst++ = (uint8_t)*name++;
    *dst = 0;
}

/* scratch layout: name @0, guid @512, data @1024.., size cell @ (SCRATCH_PAGES*4096 - 64) */
#define SC_NAME 0
#define SC_GUID 512
#define SC_DATA 1024
#define SC_DATA_MAX (SCRATCH_PAGES * PAGE_SIZE - 1024 - 128)
#define SC_CELL (SCRATCH_PAGES * PAGE_SIZE - 64)

int uefi_get_variable(const char *name, const uefi_guid_t *guid, void *buf, size_t *size, uint32_t *attr)
{
    if (!setup()) return E_NOENT;
    mutex_lock(&uefi_mtx);
    uint8_t *sc = P2V(scratch_phys);
    put_name((uint16_t *)(sc + SC_NAME), name);
    memcpy(sc + SC_GUID, guid, 16);
    uint64_t *cell = (uint64_t *)(sc + SC_CELL);
    *cell = MIN(*size, (size_t)SC_DATA_MAX);
    uint32_t *at = (uint32_t *)(sc + SC_CELL + 16);
    uint64_t st = efi_call(EFI_RT_GetVariable, scratch_phys + SC_NAME, scratch_phys + SC_GUID, scratch_phys + SC_CELL + 16,
                           scratch_phys + SC_CELL, scratch_phys + SC_DATA);
    int r = 0;
    if (st == 0) {
        size_t n = (size_t)*cell;
        if (buf) memcpy(buf, sc + SC_DATA, MIN(n, *size));
        *size = n;
        if (attr) *attr = *at;
    } else {
        r = (st & 0xFF) == 14 ? E_NOENT : E_IO;                 /* EFI_NOT_FOUND */
        if ((st & 0xFF) == 5) *size = (size_t)*cell;           /* EFI_BUFFER_TOO_SMALL */
    }
    mutex_unlock(&uefi_mtx);
    return r;
}

int uefi_set_variable(const char *name, const uefi_guid_t *guid, uint32_t attr, const void *data, size_t size)
{
    if (!setup()) return E_NOENT;
    if (size > SC_DATA_MAX) return E_INVAL;
    mutex_lock(&uefi_mtx);
    uint8_t *sc = P2V(scratch_phys);
    put_name((uint16_t *)(sc + SC_NAME), name);
    memcpy(sc + SC_GUID, guid, 16);
    if (size) memcpy(sc + SC_DATA, data, size);
    uint64_t st = efi_call(EFI_RT_SetVariable, scratch_phys + SC_NAME, scratch_phys + SC_GUID, attr, size,
                           scratch_phys + SC_DATA);
    mutex_unlock(&uefi_mtx);
    if (st) klog("uefi: SetVariable(%s) returned %#lx", name, st);
    return st ? E_IO : 0;
}

/* ---- boot entries ---- */

static void le16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void le32(uint8_t *p, uint32_t v) { le16(p, v); le16(p + 2, v >> 16); }
static void le64(uint8_t *p, uint64_t v) { le32(p, (uint32_t)v); le32(p + 4, (uint32_t)(v >> 32)); }

/* where the EFI System Partition is, as the firmware's device path wants it */
int uefi_locate_partition(struct blockdev *part, struct uefi_hd *out)
{
    struct blockdev *disk = part->parent;
    if (!disk) return E_INVAL;
    uint8_t *sec = kmalloc(512 * 2);
    if (!sec) return E_NOMEM;
    memset(out, 0, sizeof(*out));
    out->partno = (uint32_t)part->partno;
    out->start = part->start;
    out->size = part->sectors;
    int r = blk_read(disk, 1, 1, sec);
    if (!r && !memcmp(sec, "EFI PART", 8)) {
        uint64_t elba = *(uint64_t *)(sec + 72);
        uint32_t esz = *(uint32_t *)(sec + 84);
        uint32_t idx = (uint32_t)part->partno - 1;
        uint64_t lba = elba + (uint64_t)idx * esz / 512;
        uint8_t *blk = kmalloc(512);
        r = blk_read(disk, lba, 1, blk);
        if (!r) {
            const uint8_t *e = blk + ((uint64_t)idx * esz) % 512;
            memcpy(out->signature, e + 16, 16);
            out->gpt = true;
        }
        kfree(blk);
    } else if (!r) {
        r = blk_read(disk, 0, 1, sec);
        if (!r) memcpy(out->signature, sec + 0x1B8, 4);        /* the MBR's disk signature */
    }
    kfree(sec);
    return r;
}

/* EFI_LOAD_OPTION: attributes, device path length, description, device path (partition + file), no optional data */
static size_t build_load_option(uint8_t *o, const char *desc, const struct uefi_hd *hd, const char *file)
{
    size_t p = 4 + 2;
    for (const char *d = desc;; d++) { le16(o + p, (uint8_t)*d); p += 2; if (!*d) break; }
    size_t dp = p;
    /* hard drive media device path */
    o[p] = 4; o[p + 1] = 1; le16(o + p + 2, 42);
    le32(o + p + 4, hd->partno);
    le64(o + p + 8, hd->start);
    le64(o + p + 16, hd->size);
    memset(o + p + 24, 0, 16);
    memcpy(o + p + 24, hd->signature, hd->gpt ? 16 : 4);
    o[p + 40] = hd->gpt ? 2 : 1;                                /* partition format: GPT or MBR */
    o[p + 41] = hd->gpt ? 2 : 1;                                /* signature type: GUID or 32-bit MBR signature */
    p += 42;
    /* file path media device path */
    size_t fl = (strlen(file) + 1) * 2;
    o[p] = 4; o[p + 1] = 4; le16(o + p + 2, (uint32_t)(4 + fl));
    for (size_t i = 0; i <= strlen(file); i++) le16(o + p + 4 + i * 2, (uint8_t)file[i]);
    p += 4 + fl;
    o[p] = 0x7F; o[p + 1] = 0xFF; le16(o + p + 2, 4);           /* end of the device path */
    p += 4;
    le32(o, 1);                                                 /* LOAD_OPTION_ACTIVE */
    le16(o + 4, (uint32_t)(p - dp));
    return p;
}

static bool load_option_has_desc(const uint8_t *o, size_t n, const char *desc)
{
    if (n < 8) return false;
    for (size_t i = 0;; i++) {
        size_t off = 6 + i * 2;
        if (off + 2 > n) return false;
        uint16_t c = (uint16_t)(o[off] | o[off + 1] << 8);
        if (c != (uint8_t)desc[i]) return false;
        if (!c) return true;
    }
}

int uefi_add_boot_entry(const char *desc, struct blockdev *part, const char *file, int *number)
{
    if (!setup()) return E_NOENT;
    struct uefi_hd hd;
    int r = uefi_locate_partition(part, &hd);
    if (r) return r;
    uint8_t *opt = kzalloc(1024), *order = kzalloc(2048);
    if (!opt || !order) { kfree(opt); kfree(order); return E_NOMEM; }

    /* an entry from an earlier installation is replaced; otherwise take the first free number */
    int slot = -1, first_free = -1;
    for (int i = 0; i < 0x100; i++) {
        char name[12];
        snprintf(name, sizeof(name), "Boot%04X", i);
        size_t n = 1024;
        int g = uefi_get_variable(name, &global_guid, opt, &n, NULL);
        if (g == E_NOENT) { if (first_free < 0) first_free = i; continue; }
        if (!g && load_option_has_desc(opt, n, desc)) { slot = i; break; }
    }
    if (slot < 0) slot = first_free;
    if (slot < 0) { kfree(opt); kfree(order); return E_NOMEM; }

    memset(opt, 0, 1024);
    size_t len = build_load_option(opt, desc, &hd, file);
    char name[12];
    snprintf(name, sizeof(name), "Boot%04X", slot);
    r = uefi_set_variable(name, &global_guid, UEFI_NV_BS_RT, opt, len);
    if (!r) {
        /* first in BootOrder */
        size_t on = 2048;
        int g = uefi_get_variable("BootOrder", &global_guid, order, &on, NULL);
        if (g) on = 0;
        uint8_t *neworder = kzalloc(2048 + 2);
        size_t k = 0;
        le16(neworder, (uint32_t)slot);
        k = 2;
        for (size_t i = 0; i + 1 < on && k + 2 <= 2048; i += 2) {
            uint16_t v = (uint16_t)(order[i] | order[i + 1] << 8);
            if (v != slot) { le16(neworder + k, v); k += 2; }
        }
        r = uefi_set_variable("BootOrder", &global_guid, UEFI_NV_BS_RT, neworder, k);
        kfree(neworder);
    }
    kfree(opt);
    kfree(order);
    if (!r && number) *number = slot;
    return r;
}

/* the entries the firmware has, in boot order, for diagnostics */
int uefi_list_boot_entries(char out[][64], int max)
{
    if (!setup()) return 0;
    uint8_t order[256];
    size_t on = sizeof(order);
    if (uefi_get_variable("BootOrder", &global_guid, order, &on, NULL)) return 0;
    uint8_t *opt = kmalloc(1024);
    int n = 0;
    for (size_t i = 0; i + 1 < on && n < max; i += 2) {
        int num = order[i] | order[i + 1] << 8;
        char name[12];
        snprintf(name, sizeof(name), "Boot%04X", num);
        size_t sz = 1024;
        if (uefi_get_variable(name, &global_guid, opt, &sz, NULL) || sz < 8) continue;
        char d[48];
        size_t k = 0;
        for (size_t off = 6; off + 1 < sz && k < sizeof(d) - 1; off += 2) {
            uint16_t c = (uint16_t)(opt[off] | opt[off + 1] << 8);
            if (!c) break;
            d[k++] = c < 128 ? (char)c : '?';
        }
        d[k] = 0;
        snprintf(out[n++], 64, "%s  %s", name, d);
    }
    kfree(opt);
    return n;
}

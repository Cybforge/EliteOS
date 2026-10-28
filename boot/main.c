/*
 * ZenithOS UEFI bootloader.
 *
 * Loads \zenith\kernel.elf and \zenith\initrd.tar from the volume it was
 * started from, picks a graphics mode, builds the initial page tables
 * (identity + higher-half direct map + kernel image), exits boot services and
 * jumps into the 64-bit kernel with a struct bootinfo in RDI.
 */
#include "efi.h"
#include "../kernel/include/bootinfo.h"

static EFI_SYSTEM_TABLE *ST;
static EFI_BOOT_SERVICES *BS;
static EFI_HANDLE image_handle;

/* ------------------------------------------------------------------------
 * freestanding helpers (clang may emit calls to these)
 * ---------------------------------------------------------------------- */

void *memset(void *d, int c, size_t n)
{
    uint8_t *p = d;
    while (n--) *p++ = (uint8_t)c;
    return d;
}

void *memcpy(void *d, const void *s, size_t n)
{
    uint8_t *dp = d;
    const uint8_t *sp = s;
    while (n--) *dp++ = *sp++;
    return d;
}

static int guid_eq(const EFI_GUID *a, const EFI_GUID *b)
{
    const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
    for (int i = 0; i < 16; i++)
        if (x[i] != y[i]) return 0;
    return 1;
}

/* ------------------------------------------------------------------------
 * console output
 * ---------------------------------------------------------------------- */

static void puts16(const CHAR16 *s)
{
    ST->ConOut->OutputString(ST->ConOut, (CHAR16 *)s);
}

static void puts(const char *s)
{
    CHAR16 buf[128];
    size_t n = 0;
    while (*s) {
        if (*s == '\n') buf[n++] = '\r';
        buf[n++] = (CHAR16)(uint8_t)*s++;
        if (n >= 120) { buf[n] = 0; puts16(buf); n = 0; }
    }
    buf[n] = 0;
    puts16(buf);
}

static void putu(uint64_t v)
{
    char b[24];
    int i = 23;
    b[i] = 0;
    if (!v) b[--i] = '0';
    while (v) { b[--i] = (char)('0' + v % 10); v /= 10; }
    puts(b + i);
}

static void puthex(uint64_t v)
{
    char b[20];
    int i = 19;
    b[i] = 0;
    do { b[--i] = "0123456789ABCDEF"[v & 15]; v >>= 4; } while (v);
    b[--i] = 'x';
    b[--i] = '0';
    puts(b + i);
}

static void color(UINTN attr) { ST->ConOut->SetAttribute(ST->ConOut, attr); }

static void fatal(const char *msg, EFI_STATUS st)
{
    color(0x0C);
    puts("\n  boot failed: ");
    puts(msg);
    if (st) { puts(" (status "); puthex(st); puts(")"); }
    puts("\n  press any key to return to firmware\n");
    EFI_INPUT_KEY k;
    UINTN idx;
    BS->WaitForEvent(1, &ST->ConIn->WaitForKey, &idx);
    ST->ConIn->ReadKeyStroke(ST->ConIn, &k);
    for (;;) __asm__ volatile("hlt");
}

/* ------------------------------------------------------------------------
 * file loading
 * ---------------------------------------------------------------------- */

static EFI_FILE_PROTOCOL *root_dir;

static void open_root(void)
{
    EFI_GUID lip = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID sfs = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL *li;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
    EFI_STATUS st;

    st = BS->HandleProtocol(image_handle, &lip, (void **)&li);
    if (EFI_ERROR(st)) fatal("no loaded image protocol", st);
    st = BS->HandleProtocol(li->DeviceHandle, &sfs, (void **)&fs);
    if (EFI_ERROR(st)) fatal("boot volume has no file system", st);
    st = fs->OpenVolume(fs, &root_dir);
    if (EFI_ERROR(st)) fatal("cannot open boot volume", st);
}

/* Reads a whole file into freshly allocated pages. Returns NULL if absent. */
static void *load_file(CHAR16 *path, uint64_t *size_out, EFI_MEMORY_TYPE type)
{
    EFI_FILE_PROTOCOL *f;
    EFI_STATUS st = root_dir->Open(root_dir, &f, path, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(st)) return NULL;

    EFI_GUID fi_guid = EFI_FILE_INFO_GUID;
    uint8_t info_buf[512];
    UINTN info_size = sizeof(info_buf);
    st = f->GetInfo(f, &fi_guid, &info_size, info_buf);
    if (EFI_ERROR(st)) fatal("GetInfo failed", st);
    uint64_t size = ((EFI_FILE_INFO *)info_buf)->FileSize;

    EFI_PHYSICAL_ADDRESS addr = 0;
    UINTN pages = (size + 4095) / 4096;
    if (!pages) pages = 1;
    st = BS->AllocatePages(AllocateAnyPages, type, pages, &addr);
    if (EFI_ERROR(st)) fatal("out of memory loading file", st);

    uint8_t *p = (uint8_t *)addr;
    uint64_t done = 0;
    while (done < size) {
        UINTN chunk = size - done;
        if (chunk > (1u << 20)) chunk = 1u << 20;
        st = f->Read(f, &chunk, p + done);
        if (EFI_ERROR(st)) fatal("read error", st);
        if (!chunk) break;
        done += chunk;
    }
    f->Close(f);
    *size_out = size;
    return p;
}

/* ------------------------------------------------------------------------
 * boot.cfg (optional):  resolution=1280x800
 * ---------------------------------------------------------------------- */

static uint32_t cfg_w, cfg_h;

static uint32_t parse_u(const char **s)
{
    uint32_t v = 0;
    while (**s >= '0' && **s <= '9') v = v * 10 + (uint32_t)(*(*s)++ - '0');
    return v;
}

static void read_config(void)
{
    uint64_t size;
    char *cfg = load_file(L"\\zenith\\boot.cfg", &size, EfiLoaderData);
    if (!cfg) return;
    for (uint64_t i = 0; i < size; i++) {
        const char *key = "resolution=";
        uint64_t k = 0;
        while (key[k] && i + k < size && cfg[i + k] == key[k]) k++;
        if (!key[k]) {
            const char *s = cfg + i + k;
            cfg_w = parse_u(&s);
            if (*s == 'x' || *s == 'X') { s++; cfg_h = parse_u(&s); }
            break;
        }
    }
    BS->FreePages((EFI_PHYSICAL_ADDRESS)cfg, (size + 4095) / 4096);
}

static void write_config(uint32_t w, uint32_t h)
{
    EFI_FILE_PROTOCOL *f;
    EFI_STATUS st = root_dir->Open(root_dir, &f, L"\\zenith\\boot.cfg",
                                   EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
    if (EFI_ERROR(st)) return;   /* read-only media (CD) - fine */
    char buf[64];
    int n = 0;
    const char *k = "resolution=";
    while (*k) buf[n++] = *k++;
    char tmp[12];
    int t = 0;
    do { tmp[t++] = (char)('0' + w % 10); w /= 10; } while (w);
    while (t) buf[n++] = tmp[--t];
    buf[n++] = 'x';
    do { tmp[t++] = (char)('0' + h % 10); h /= 10; } while (h);
    while (t) buf[n++] = tmp[--t];
    buf[n++] = '\n';
    UINTN len = (UINTN)n;
    f->SetPosition(f, 0);
    f->Write(f, &len, buf);
    f->Close(f);
}

/* ------------------------------------------------------------------------
 * graphics mode selection
 * ---------------------------------------------------------------------- */

static EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;

static int mode_usable(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi)
{
    if (mi->PixelFormat == PixelBltOnly) return 0;
    if (mi->PixelFormat == PixelBitMask) {
        /* accept only the two 32-bit byte orders we support */
        if (!((mi->PixelInformation.RedMask == 0xFF0000 && mi->PixelInformation.BlueMask == 0xFF) ||
              (mi->PixelInformation.RedMask == 0xFF && mi->PixelInformation.BlueMask == 0xFF0000)))
            return 0;
    }
    return mi->HorizontalResolution >= 800 && mi->VerticalResolution >= 600;
}

static int query(uint32_t m, uint32_t *w, uint32_t *h)
{
    UINTN sz;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi;
    if (EFI_ERROR(gop->QueryMode(gop, m, &sz, &mi))) return 0;
    if (!mode_usable(mi)) return 0;
    *w = mi->HorizontalResolution;
    *h = mi->VerticalResolution;
    return 1;
}

static int find_mode(uint32_t w, uint32_t h)
{
    for (uint32_t m = 0; m < gop->Mode->MaxMode; m++) {
        uint32_t mw, mh;
        if (query(m, &mw, &mh) && mw == w && mh == h) return (int)m;
    }
    return -1;
}

static int pick_default_mode(void)
{
    int m;
    if (cfg_w && (m = find_mode(cfg_w, cfg_h)) >= 0) return m;

    /* Real hardware usually boots in the panel's native mode - keep it. */
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *cur = gop->Mode->Info;
    if (mode_usable(cur) && cur->HorizontalResolution >= 1024 &&
        cur->HorizontalResolution <= 2560 && cur->VerticalResolution >= 700)
        return (int)gop->Mode->Mode;

    static const uint32_t pref[][2] = {
        { 1280, 800 }, { 1366, 768 }, { 1280, 720 }, { 1440, 900 }, { 1024, 768 },
    };
    for (unsigned i = 0; i < sizeof(pref) / sizeof(pref[0]); i++)
        if ((m = find_mode(pref[i][0], pref[i][1])) >= 0) return m;

    /* Otherwise the largest mode that is not absurdly big. */
    int best = -1;
    uint64_t best_area = 0;
    for (uint32_t i = 0; i < gop->Mode->MaxMode; i++) {
        uint32_t w, h;
        if (!query(i, &w, &h) || w > 1920 || h > 1200) continue;
        if ((uint64_t)w * h > best_area) { best_area = (uint64_t)w * h; best = (int)i; }
    }
    return best;
}

static int mode_menu(int current)
{
    uint32_t list[64];
    int n = 0, sel = 0;
    for (uint32_t m = 0; m < gop->Mode->MaxMode && n < 64; m++) {
        uint32_t w, h;
        if (!query(m, &w, &h)) continue;
        if ((int)m == current) sel = n;
        list[n++] = m;
    }
    if (!n) return current;

    for (;;) {
        ST->ConOut->ClearScreen(ST->ConOut);
        color(EFI_LIGHTMAGENTA);
        puts("\n  ZenithOS - display mode\n\n");
        color(EFI_LIGHTGRAY);
        puts("  Up/Down to choose, Enter to boot, Esc to cancel\n\n");
        int first = sel > 14 ? sel - 14 : 0;
        for (int i = first; i < n && i < first + 16; i++) {
            uint32_t w, h;
            query(list[i], &w, &h);
            color(i == sel ? (EFI_WHITE | EFI_BACKGROUND_BLUE) : EFI_LIGHTGRAY);
            puts(i == sel ? "   > " : "     ");
            putu(w);
            puts(" x ");
            putu(h);
            puts("   \n");
        }
        color(EFI_LIGHTGRAY);

        EFI_INPUT_KEY k;
        UINTN idx;
        BS->WaitForEvent(1, &ST->ConIn->WaitForKey, &idx);
        if (EFI_ERROR(ST->ConIn->ReadKeyStroke(ST->ConIn, &k))) continue;
        if (k.ScanCode == SCAN_UP && sel > 0) sel--;
        else if (k.ScanCode == SCAN_DOWN && sel < n - 1) sel++;
        else if (k.ScanCode == SCAN_ESC) return current;
        else if (k.UnicodeChar == '\r' || k.UnicodeChar == '\n') {
            uint32_t w, h;
            query(list[sel], &w, &h);
            write_config(w, h);
            return (int)list[sel];
        }
    }
}

/* ------------------------------------------------------------------------
 * ELF loading
 * ---------------------------------------------------------------------- */

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    uint32_t p_type, p_flags;
    uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
} Elf64_Phdr;

#define PT_LOAD 1

/* ------------------------------------------------------------------------
 * page tables
 * ---------------------------------------------------------------------- */

#define PTE_P   0x001ull
#define PTE_W   0x002ull
#define PTE_PS  0x080ull
#define ADDR_MASK 0x000FFFFFFFFFF000ull

static uint64_t *pt_pool;
static UINTN pt_pool_left;

static uint64_t *pt_alloc(void)
{
    if (!pt_pool_left) fatal("page table pool exhausted", 0);
    uint64_t *p = pt_pool;
    pt_pool += 512;
    pt_pool_left--;
    memset(p, 0, 4096);
    return p;
}

static uint64_t *pt_next(uint64_t *table, unsigned idx)
{
    if (!(table[idx] & PTE_P))
        table[idx] = (uint64_t)pt_alloc() | PTE_P | PTE_W;
    return (uint64_t *)(table[idx] & ADDR_MASK);
}

static void map_4k(uint64_t *pml4, uint64_t va, uint64_t pa)
{
    uint64_t *pdpt = pt_next(pml4, (va >> 39) & 511);
    uint64_t *pd = pt_next(pdpt, (va >> 30) & 511);
    uint64_t *pt = pt_next(pd, (va >> 21) & 511);
    pt[(va >> 12) & 511] = pa | PTE_P | PTE_W;
}

static void map_2m(uint64_t *pml4, uint64_t va, uint64_t pa)
{
    uint64_t *pdpt = pt_next(pml4, (va >> 39) & 511);
    uint64_t *pd = pt_next(pdpt, (va >> 30) & 511);
    pd[(va >> 21) & 511] = pa | PTE_P | PTE_W | PTE_PS;
}

/* ------------------------------------------------------------------------
 * memory map translation
 * ---------------------------------------------------------------------- */

static uint32_t translate_type(uint32_t t)
{
    switch (t) {
    case EfiConventionalMemory:
    case EfiBootServicesCode:
    case EfiBootServicesData:
    case EfiLoaderCode:
        return MEM_USABLE;
    case EfiLoaderData:
        return MEM_BOOT;
    case EfiACPIReclaimMemory:
        return MEM_ACPI_RECL;
    case EfiACPIMemoryNVS:
        return MEM_ACPI_NVS;
    default:
        return MEM_RESERVED;
    }
}

/* ------------------------------------------------------------------------
 * entry
 * ---------------------------------------------------------------------- */

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ih, EFI_SYSTEM_TABLE *st)
{
    EFI_STATUS s;
    ST = st;
    BS = st->BootServices;
    image_handle = ih;

    BS->SetWatchdogTimer(0, 0, 0, NULL);
    ST->ConOut->ClearScreen(ST->ConOut);
    color(EFI_LIGHTMAGENTA);
    puts("\n  Z E N I T H O S\n");
    color(EFI_LIGHTGRAY);
    puts("  UEFI boot manager\n\n");

    open_root();
    read_config();

    EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    s = BS->LocateProtocol(&gop_guid, NULL, (void **)&gop);
    if (EFI_ERROR(s)) fatal("no graphics output (GOP) available", s);

    int mode = pick_default_mode();
    if (mode < 0) fatal("no usable 32-bit graphics mode", 0);

    /* Short window to open the display-mode menu. */
    {
        uint32_t w = 0, h = 0;
        query((uint32_t)mode, &w, &h);
        puts("  display ");
        putu(w); puts("x"); putu(h);
        puts(" - press any key for display options ");
        int pressed = 0;
        for (int i = 0; i < 12 && !pressed; i++) {
            EFI_INPUT_KEY k;
            if (!EFI_ERROR(ST->ConIn->ReadKeyStroke(ST->ConIn, &k))) pressed = 1;
            else { BS->Stall(100000); if (i % 3 == 2) puts("."); }
        }
        puts("\n");
        if (pressed) mode = mode_menu(mode);
    }

    /* Load kernel and initrd. */
    puts("  loading kernel...\n");
    uint64_t ksize;
    uint8_t *kfile = load_file(L"\\zenith\\kernel.elf", &ksize, EfiLoaderData);
    if (!kfile) fatal("\\zenith\\kernel.elf not found", 0);

    Elf64_Ehdr *eh = (Elf64_Ehdr *)kfile;
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' || eh->e_ident[2] != 'L' ||
        eh->e_ident[3] != 'F' || eh->e_ident[4] != 2 || eh->e_machine != 0x3E)
        fatal("kernel.elf is not an x86-64 ELF", 0);

    uint64_t vmin = ~0ull, vmax = 0;
    for (unsigned i = 0; i < eh->e_phnum; i++) {
        Elf64_Phdr *ph = (Elf64_Phdr *)(kfile + eh->e_phoff + (uint64_t)i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD) continue;
        if (ph->p_vaddr < vmin) vmin = ph->p_vaddr;
        if (ph->p_vaddr + ph->p_memsz > vmax) vmax = ph->p_vaddr + ph->p_memsz;
    }
    vmin &= ~0xFFFull;
    vmax = (vmax + 0xFFF) & ~0xFFFull;
    if (vmin != KERNEL_VBASE) fatal("kernel not linked at KERNEL_VBASE", 0);

    uint64_t kpages = (vmax - vmin) / 4096;
    EFI_PHYSICAL_ADDRESS kphys = 0;
    /* Keep the kernel below 4 GiB: the SMP trampoline relies on it. */
    kphys = 0xFFFFFFFFull;
    s = BS->AllocatePages(AllocateMaxAddress, EfiLoaderData, kpages, &kphys);
    if (EFI_ERROR(s)) fatal("cannot allocate kernel memory", s);
    memset((void *)kphys, 0, kpages * 4096);
    for (unsigned i = 0; i < eh->e_phnum; i++) {
        Elf64_Phdr *ph = (Elf64_Phdr *)(kfile + eh->e_phoff + (uint64_t)i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD) continue;
        memcpy((uint8_t *)kphys + (ph->p_vaddr - vmin), kfile + ph->p_offset, ph->p_filesz);
    }
    uint64_t kentry = eh->e_entry;
    /* the kernel file stays in memory (for the installer), and so does this loader */
    uint64_t lsize = 0;
    void *lfile = load_file(L"\\EFI\\BOOT\\BOOTX64.EFI", &lsize, EfiLoaderData);

    puts("  loading initrd...\n");
    uint64_t isize = 0;
    void *initrd = load_file(L"\\zenith\\initrd.tar", &isize, EfiLoaderData);
    if (!initrd) { puts("  (no initrd)\n"); isize = 0; }

    /* Boot info. */
    EFI_PHYSICAL_ADDRESS bi_phys = 0;
    UINTN bi_pages = (sizeof(struct bootinfo) + 4095) / 4096;
    s = BS->AllocatePages(AllocateAnyPages, EfiLoaderData, bi_pages, &bi_phys);
    if (EFI_ERROR(s)) fatal("cannot allocate boot info", s);
    struct bootinfo *bi = (struct bootinfo *)bi_phys;
    memset(bi, 0, bi_pages * 4096);
    bi->magic = BOOTINFO_MAGIC;
    bi->initrd_phys = (uint64_t)initrd;
    bi->initrd_size = isize;
    bi->kernel_phys = kphys;
    bi->kernel_size = kpages * 4096;
    bi->kernel_file_phys = (uint64_t)kfile;
    bi->kernel_file_size = ksize;
    bi->loader_file_phys = (uint64_t)lfile;
    bi->loader_file_size = lfile ? lsize : 0;
    {
        const CHAR16 *v = ST->FirmwareVendor;
        int i = 0;
        while (v && v[i] && i < 63) { bi->firmware_vendor[i] = (char)(v[i] < 128 ? v[i] : '?'); i++; }
    }

    /* The monitor's EDID, so the kernel knows which modes and refresh rates it supports. */
    {
        EFI_GUID ids[2] = { EFI_EDID_ACTIVE_PROTOCOL_GUID, EFI_EDID_DISCOVERED_PROTOCOL_GUID };
        for (int i = 0; i < 2 && !bi->edid_size; i++) {
            EFI_EDID_PROTOCOL *e = NULL;
            if (!EFI_ERROR(BS->LocateProtocol(&ids[i], NULL, (void **)&e)) && e && e->Edid && e->SizeOfEdid >= 128) {
                bi->edid_size = e->SizeOfEdid > sizeof(bi->edid) ? sizeof(bi->edid) : e->SizeOfEdid;
                memcpy(bi->edid, e->Edid, bi->edid_size);
            }
        }
    }

    /* ACPI RSDP. */
    {
        EFI_GUID a2 = EFI_ACPI_20_TABLE_GUID, a1 = EFI_ACPI_10_TABLE_GUID;
        for (UINTN i = 0; i < ST->NumberOfTableEntries; i++) {
            EFI_CONFIGURATION_TABLE *ct = &ST->ConfigurationTable[i];
            if (guid_eq(&ct->VendorGuid, &a2)) { bi->rsdp_phys = (uint64_t)ct->VendorTable; break; }
            if (guid_eq(&ct->VendorGuid, &a1) && !bi->rsdp_phys) bi->rsdp_phys = (uint64_t)ct->VendorTable;
        }
    }

    /* Kernel boot stack (the kernel switches to its own immediately, but it
     * must not start on firmware memory that it will reclaim). */
    EFI_PHYSICAL_ADDRESS stack_phys = 0;
    s = BS->AllocatePages(AllocateAnyPages, EfiLoaderData, 4, &stack_phys);
    if (EFI_ERROR(s)) fatal("cannot allocate stack", s);

    /* Set the graphics mode now - text output stops being visible after this. */
    s = gop->SetMode(gop, (uint32_t)mode);
    if (EFI_ERROR(s)) fatal("cannot set graphics mode", s);
    {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi = gop->Mode->Info;
        bi->fb.phys = gop->Mode->FrameBufferBase;
        bi->fb.width = mi->HorizontalResolution;
        bi->fb.height = mi->VerticalResolution;
        bi->fb.pitch = mi->PixelsPerScanLine * 4;
        if (mi->PixelFormat == PixelRedGreenBlueReserved8BitPerColor ||
            (mi->PixelFormat == PixelBitMask && mi->PixelInformation.RedMask == 0xFF))
            bi->fb.format = FB_FORMAT_RGBX;
        else
            bi->fb.format = FB_FORMAT_BGRX;
    }

    /* Size the direct map: all of RAM, at least 4 GiB, plus the framebuffer. */
    UINTN mmap_size = 0, map_key, desc_size;
    uint32_t desc_ver;
    BS->GetMemoryMap(&mmap_size, NULL, &map_key, &desc_size, &desc_ver);
    mmap_size += 64 * desc_size;
    EFI_MEMORY_DESCRIPTOR *mmap;
    s = BS->AllocatePool(EfiLoaderData, mmap_size, (void **)&mmap);
    if (EFI_ERROR(s)) fatal("cannot allocate memory map", s);
    UINTN msz = mmap_size;
    s = BS->GetMemoryMap(&msz, mmap, &map_key, &desc_size, &desc_ver);
    if (EFI_ERROR(s)) fatal("GetMemoryMap failed", s);

    uint64_t top = 0x100000000ull;
    for (UINTN off = 0; off < msz; off += desc_size) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)((uint8_t *)mmap + off);
        uint32_t t = translate_type(d->Type);
        if (t == MEM_RESERVED) continue;
        uint64_t end = d->PhysicalStart + d->NumberOfPages * 4096;
        if (end > top) top = end;
    }
    uint64_t fb_end = bi->fb.phys + (uint64_t)bi->fb.pitch * bi->fb.height;
    if (fb_end > top) top = fb_end;
    top = (top + 0x3FFFFFFFull) & ~0x3FFFFFFFull;     /* round to 1 GiB */
    bi->hhdm_size = top;

    /* Page table pool: PDs for the direct map (shared by identity + HHDM),
     * PDPTs, and PTs for the kernel image. */
    UINTN n_pd = (UINTN)(top >> 30);
    UINTN n_pdpt = (n_pd + 511) / 512;
    UINTN n_pt = (UINTN)(kpages + 511) / 512 + 2;
    UINTN pool_pages = 1 + n_pdpt * 2 + n_pd * 2 + n_pt + 4;
    EFI_PHYSICAL_ADDRESS pool = 0;
    s = BS->AllocatePages(AllocateAnyPages, EfiLoaderData, pool_pages, &pool);
    if (EFI_ERROR(s)) fatal("cannot allocate page tables", s);
    pt_pool = (uint64_t *)pool;
    pt_pool_left = pool_pages;

    uint64_t *pml4 = pt_alloc();
    for (uint64_t pa = 0; pa < top; pa += 0x200000) {
        map_2m(pml4, pa, pa);                 /* identity (for the jump) */
        map_2m(pml4, HHDM_BASE + pa, pa);     /* direct map */
    }
    for (uint64_t i = 0; i < kpages; i++)
        map_4k(pml4, KERNEL_VBASE + i * 4096, kphys + i * 4096);

    puts("  starting kernel\n");

    /* Final memory map + ExitBootServices. The allocation above may have
     * changed the map, so fetch it again; retry if the key went stale. */
    for (int tries = 0; ; tries++) {
        msz = mmap_size;
        s = BS->GetMemoryMap(&msz, mmap, &map_key, &desc_size, &desc_ver);
        if (EFI_ERROR(s)) fatal("GetMemoryMap failed", s);
        s = BS->ExitBootServices(ih, map_key);
        if (!EFI_ERROR(s)) break;
        if (tries > 4) fatal("ExitBootServices failed", s);
    }

    /* No firmware calls from here on. Convert and merge the memory map. */
    uint32_t n = 0;
    for (UINTN off = 0; off < msz; off += desc_size) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)((uint8_t *)mmap + off);
        uint32_t t = translate_type(d->Type);
        uint64_t base = d->PhysicalStart, len = d->NumberOfPages * 4096;
        if (n && bi->mmap[n - 1].type == t && bi->mmap[n - 1].base + bi->mmap[n - 1].length == base) {
            bi->mmap[n - 1].length += len;
            continue;
        }
        if (n >= BOOT_MMAP_MAX) break;
        bi->mmap[n].base = base;
        bi->mmap[n].length = len;
        bi->mmap[n].type = t;
        n++;
    }
    bi->mmap_count = n;

    /* the regions the firmware's runtime services live in (no SetVirtualAddressMap: they stay at their physical addresses) */
    bi->efi_runtime = (uint64_t)ST->RuntimeServices;
    for (UINTN off = 0; off < msz; off += desc_size) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)((uint8_t *)mmap + off);
        if (!(d->Attribute & 0x8000000000000000ull) || bi->efi_rt_count >= BOOT_RT_MAX) continue;   /* EFI_MEMORY_RUNTIME */
        struct boot_rt_region *r = &bi->efi_rt[bi->efi_rt_count++];
        r->phys = d->PhysicalStart;
        r->length = d->NumberOfPages * 4096;
        r->type = d->Type;
        r->mmio = d->Type == EfiMemoryMappedIO || d->Type == EfiMemoryMappedIOPortSpace;
    }

    uint64_t bi_virt = HHDM_BASE + bi_phys;
    uint64_t stack_top = HHDM_BASE + stack_phys + 4 * 4096;
    __asm__ volatile(
        "cli\n\t"
        "mov %0, %%cr3\n\t"
        "mov %1, %%rdi\n\t"
        "mov %2, %%rsp\n\t"
        "xor %%rbp, %%rbp\n\t"
        "jmp *%3\n\t"
        :: "r"((uint64_t)pml4), "r"(bi_virt), "r"(stack_top), "r"(kentry)
        : "rdi", "memory");
    __builtin_unreachable();
}

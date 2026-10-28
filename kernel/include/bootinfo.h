/*
 * ZenithOS boot protocol.
 *
 * Shared between the UEFI bootloader (boot/) and the kernel. The bootloader
 * fills one of these, maps it through the higher-half direct map and passes
 * its virtual address to the kernel entry point in RDI.
 */
#ifndef ZENITH_BOOTINFO_H
#define ZENITH_BOOTINFO_H

#include <stdint.h>

#define BOOTINFO_MAGIC   0x5A454E4954484F53ull /* "ZENITHOS" */

/* Virtual layout set up by the bootloader. */
#define HHDM_BASE        0xFFFF800000000000ull  /* all RAM mapped here */
#define KERNEL_VBASE     0xFFFFFFFF80000000ull  /* kernel image */

enum {
    MEM_USABLE      = 1,  /* free RAM */
    MEM_RESERVED    = 2,  /* firmware, MMIO, holes */
    MEM_ACPI_RECL   = 3,  /* ACPI tables, reclaimable after parsing */
    MEM_ACPI_NVS    = 4,
    MEM_BOOT        = 5,  /* kernel image, initrd, boot info, page tables */
};

struct boot_mmap_entry {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t _pad;
};

enum {
    FB_FORMAT_BGRX = 0,   /* byte order B,G,R,X -> 0x00RRGGBB as uint32 */
    FB_FORMAT_RGBX = 1,   /* byte order R,G,B,X */
};

struct boot_framebuffer {
    uint64_t phys;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;       /* bytes per scanline */
    uint32_t format;
};

#define BOOT_MMAP_MAX 512

struct bootinfo {
    uint64_t magic;
    struct boot_framebuffer fb;
    uint64_t rsdp_phys;           /* ACPI RSDP, 0 if none */
    uint64_t initrd_phys;
    uint64_t initrd_size;
    uint64_t kernel_phys;         /* physical address of KERNEL_VBASE */
    uint64_t kernel_size;
    uint64_t hhdm_size;           /* bytes of physical memory mapped at HHDM_BASE */
    uint32_t mmap_count;
    uint32_t boot_flags;
    char     firmware_vendor[64];
    uint32_t edid_size;           /* the monitor's EDID as the firmware read it, 0 if it did not */
    uint8_t  edid[256];
    /* raw boot files, kept in memory so the installer can copy them to a disk */
    uint64_t kernel_file_phys;
    uint64_t kernel_file_size;
    uint64_t loader_file_phys;
    uint64_t loader_file_size;
    struct boot_mmap_entry mmap[BOOT_MMAP_MAX];
};

#endif

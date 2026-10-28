#ifndef ZENITH_UEFI_H
#define ZENITH_UEFI_H

/* firmware variables and boot entries through the UEFI runtime services (dev/uefi.c) */
#include <kernel.h>
#include <block.h>

typedef struct {
    uint32_t a;
    uint16_t b, c;
    uint8_t d[8];
} uefi_guid_t;

#define UEFI_NV_BS_RT 7                  /* non-volatile, boot services, runtime */

struct uefi_hd {
    uint32_t partno;
    uint64_t start, size;               /* in sectors */
    uint8_t signature[16];              /* GPT partition GUID, or the 32-bit MBR disk signature */
    bool gpt;
};

bool uefi_available(void);
int  uefi_get_variable(const char *name, const uefi_guid_t *guid, void *buf, size_t *size, uint32_t *attr);
int  uefi_set_variable(const char *name, const uefi_guid_t *guid, uint32_t attr, const void *data, size_t size);
int  uefi_locate_partition(struct blockdev *part, struct uefi_hd *out);
/* add (or replace) a boot entry that starts `file` (like "\EFI\BOOT\BOOTX64.EFI") from `part` and put it first */
int  uefi_add_boot_entry(const char *desc, struct blockdev *part, const char *file, int *number);
int  uefi_list_boot_entries(char out[][64], int max);

#endif

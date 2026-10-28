#ifndef ZENITH_BLOCK_H
#define ZENITH_BLOCK_H

#include <kernel.h>

/*
 * Block devices: whole disks (sda, sdb, ...) registered by drivers, and
 * their partitions (sda1, ...) found in the GPT or MBR. Sectors are 512 bytes.
 */
#define SECTOR_SIZE 512

struct blockdev {
    char name[16];
    char model[48];
    uint64_t sectors;
    struct blockdev *parent;     /* partitions: the whole disk */
    uint64_t start;              /* partitions: first sector on the parent */
    int partno;
    uint8_t type_guid[16];       /* GPT partition type (zero for MBR) */
    uint8_t mbr_type;
    int (*read)(struct blockdev *d, uint64_t lba, uint32_t count, void *buf);
    int (*write)(struct blockdev *d, uint64_t lba, uint32_t count, const void *buf);
    int (*flush)(struct blockdev *d);
    void *priv;
    bool busy;                   /* mounted or being installed to */
};

void blk_register(struct blockdev *d);           /* whole disk; scans partitions */
void blk_rescan(struct blockdev *disk);          /* drop and re-read the partitions */
int  blk_count(void);
struct blockdev *blk_get(int i);
struct blockdev *blk_find(const char *name);
int  blk_read(struct blockdev *d, uint64_t lba, uint32_t count, void *buf);
int  blk_write(struct blockdev *d, uint64_t lba, uint32_t count, const void *buf);
int  blk_flush(struct blockdev *d);
void blk_format_size(uint64_t bytes, char *out, size_t n);

uint32_t crc32(uint32_t crc, const void *data, size_t len);

/* ahci.c */
void ahci_init(void);

/* fat.c */
int  fat_mount(struct blockdev *d, const char *path);
int  fat_mkfs(struct blockdev *d, const char *label);
bool fat_probe(struct blockdev *d, char *label, size_t n);
void fat_automount(void);

/* gpt helpers used by the installer */
int  gpt_create_single(struct blockdev *disk, const char *part_name, bool esp);
int  part_make_bootable(struct blockdev *part);

/* installer (apps/installer.c) */
struct install_progress {
    volatile int step, nsteps;
    volatile int pct;
    volatile bool done, failed;
    char msg[160];
    char note[160];                      /* something worth telling the user afterwards (no driver for the GPU ...) */
    bool boot_entry;                     /* the firmware boots the new system first */
};

/* everything the installer asks for */
struct install_opts {
    struct blockdev *disk;
    bool erase;                          /* new partition table instead of adding to the FAT32 volume */
    char language[16], keyboard[16], timezone[64], hostname[40];
    char user[32], fullname[64], password[64];
    bool autologin;
    bool gpu_driver;                     /* download the graphics driver for this computer's adapter */
};
void install_defaults(struct install_opts *o);          /* the running system's language, keyboard, time zone */
int  zenith_install(const struct install_opts *o, struct install_progress *p);

#endif

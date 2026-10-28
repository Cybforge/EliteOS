/*
 * Block device registry and partition tables (GPT and MBR).
 */
#include <block.h>
#include <mm.h>
#include <x86.h>
#include <spinlock.h>
#include <vfs.h>

#define MAX_BLK 32

static struct blockdev *devs[MAX_BLK];
static int ndevs;
static spinlock_t blk_lock = SPINLOCK_INIT("blk");

/* ------------------------------------------------------------------------
 * CRC32 (IEEE), used by GPT
 * ---------------------------------------------------------------------- */

uint32_t crc32(uint32_t crc, const void *data, size_t len)
{
    static uint32_t table[256];
    static bool ready;
    if (!ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    const uint8_t *p = data;
    crc = ~crc;
    while (len--) crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* ------------------------------------------------------------------------
 * registry
 * ---------------------------------------------------------------------- */

int blk_count(void) { return ndevs; }
struct blockdev *blk_get(int i) { return i >= 0 && i < ndevs ? devs[i] : NULL; }

struct blockdev *blk_find(const char *name)
{
    for (int i = 0; i < ndevs; i++)
        if (!strcmp(devs[i]->name, name)) return devs[i];
    return NULL;
}

static void add(struct blockdev *d)
{
    spin_lock(&blk_lock);
    if (ndevs < MAX_BLK) devs[ndevs++] = d;
    spin_unlock(&blk_lock);
}

int blk_read(struct blockdev *d, uint64_t lba, uint32_t count, void *buf)
{
    if (!count) return 0;
    if (lba + count > d->sectors) return E_INVAL;
    return d->read(d, lba, count, buf);
}

int blk_write(struct blockdev *d, uint64_t lba, uint32_t count, const void *buf)
{
    if (!count) return 0;
    if (lba + count > d->sectors || !d->write) return E_INVAL;
    return d->write(d, lba, count, buf);
}

int blk_flush(struct blockdev *d)
{
    while (d->parent) d = d->parent;
    return d->flush ? d->flush(d) : 0;
}

void blk_format_size(uint64_t bytes, char *out, size_t n)
{
    const char *u[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    int i = 0;
    uint64_t whole = bytes, frac = 0;
    while (whole >= 1024 && i < 4) { frac = (whole % 1024) * 10 / 1024; whole /= 1024; i++; }
    if (i && whole < 100) snprintf(out, n, "%lu.%lu %s", whole, frac, u[i]);
    else snprintf(out, n, "%lu %s", whole, u[i]);
}

/* ------------------------------------------------------------------------
 * partitions
 * ---------------------------------------------------------------------- */

static int part_read(struct blockdev *d, uint64_t lba, uint32_t count, void *buf)
{
    return blk_read(d->parent, d->start + lba, count, buf);
}

static int part_write(struct blockdev *d, uint64_t lba, uint32_t count, const void *buf)
{
    return blk_write(d->parent, d->start + lba, count, buf);
}

static int part_flush(struct blockdev *d) { return blk_flush(d->parent); }

static void add_partition(struct blockdev *disk, int no, uint64_t start, uint64_t count,
                          const uint8_t *guid, uint8_t mbr_type)
{
    if (!count || start + count > disk->sectors) return;
    struct blockdev *p = kzalloc(sizeof(*p));
    snprintf(p->name, sizeof(p->name), "%s%d", disk->name, no);
    strlcpy(p->model, disk->model, sizeof(p->model));
    p->parent = disk;
    p->start = start;
    p->sectors = count;
    p->partno = no;
    if (guid) memcpy(p->type_guid, guid, 16);
    p->mbr_type = mbr_type;
    p->read = part_read;
    p->write = part_write;
    p->flush = part_flush;
    add(p);
    char sz[24];
    blk_format_size(count * SECTOR_SIZE, sz, sizeof(sz));
    klog("blk: %s: partition %d, %s at sector %lu", p->name, no, sz, start);
}

static bool scan_gpt(struct blockdev *disk, uint8_t *buf)
{
    if (blk_read(disk, 1, 1, buf)) return false;
    if (memcmp(buf, "EFI PART", 8)) return false;
    uint64_t ents_lba = *(uint64_t *)(buf + 72);
    uint32_t nents = *(uint32_t *)(buf + 80);
    uint32_t esz = *(uint32_t *)(buf + 84);
    if (esz < 128 || esz > 512 || nents > 256) return false;
    uint32_t bytes = nents * esz;
    uint32_t secs = (bytes + SECTOR_SIZE - 1) / SECTOR_SIZE;
    uint8_t *ents = kmalloc((size_t)secs * SECTOR_SIZE);
    if (blk_read(disk, ents_lba, secs, ents)) { kfree(ents); return false; }
    int no = 0;
    for (uint32_t i = 0; i < nents; i++) {
        uint8_t *e = ents + (size_t)i * esz;
        static const uint8_t zero[16];
        no++;
        if (!memcmp(e, zero, 16)) continue;
        uint64_t first = *(uint64_t *)(e + 32), last = *(uint64_t *)(e + 40);
        if (last >= first) add_partition(disk, no, first, last - first + 1, e, 0);
    }
    kfree(ents);
    return true;
}

static void scan(struct blockdev *disk)
{
    uint8_t *buf = kmalloc(SECTOR_SIZE);
    if (blk_read(disk, 0, 1, buf) || buf[510] != 0x55 || buf[511] != 0xAA) { kfree(buf); return; }
    uint8_t mbr[64];
    memcpy(mbr, buf + 446, 64);
    bool protective = false;
    for (int i = 0; i < 4; i++) if (mbr[i * 16 + 4] == 0xEE) protective = true;
    if (protective && scan_gpt(disk, buf)) { kfree(buf); return; }
    /* a FAT boot sector (no partition table) has jump code at offset 0 */
    if ((buf[0] == 0xEB || buf[0] == 0xE9) && !memcmp(buf + 82, "FAT32", 5)) { kfree(buf); return; }
    for (int i = 0; i < 4; i++) {
        uint8_t *e = mbr + i * 16;
        uint8_t type = e[4];
        uint32_t start = *(uint32_t *)(e + 8), count = *(uint32_t *)(e + 12);
        if (!type || type == 0x05 || type == 0x0F || type == 0x85) continue;
        add_partition(disk, i + 1, start, count, NULL, type);
    }
    kfree(buf);
}

void blk_register(struct blockdev *d)
{
    add(d);
    char sz[24];
    blk_format_size(d->sectors * SECTOR_SIZE, sz, sizeof(sz));
    klog("blk: %s: %s, %s", d->name, d->model, sz);
    scan(d);
}

void blk_rescan(struct blockdev *disk)
{
    spin_lock(&blk_lock);
    int o = 0;
    for (int i = 0; i < ndevs; i++) {
        if (devs[i]->parent == disk && !devs[i]->busy) { kfree(devs[i]); continue; }
        devs[o++] = devs[i];
    }
    ndevs = o;
    spin_unlock(&blk_lock);
    scan(disk);
}

/* ------------------------------------------------------------------------
 * writing a fresh GPT with one partition covering the disk
 * ---------------------------------------------------------------------- */

static void random_guid(uint8_t *g)
{
    static uint64_t state;
    if (!state) state = rdtsc() | 1;
    for (int i = 0; i < 16; i++) {
        state ^= state << 13; state ^= state >> 7; state ^= state << 17;
        g[i] = (uint8_t)(state >> 24);
    }
    g[7] = (uint8_t)((g[7] & 0x0F) | 0x40);   /* version 4 */
    g[8] = (uint8_t)((g[8] & 0x3F) | 0x80);
}

static const uint8_t guid_esp[16] = { 0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
                                      0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B };
static const uint8_t guid_basic[16] = { 0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44,
                                        0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7 };

int gpt_create_single(struct blockdev *disk, const char *part_name, bool esp)
{
    uint64_t total = disk->sectors;
    if (total < 34 * 2 + 4096) return E_INVAL;
    uint64_t first = 2048, last_usable = total - 34;
    uint64_t last = ((last_usable + 1) / 2048) * 2048 - 1;
    if (last <= first) last = last_usable;

    uint8_t *sec = kzalloc(SECTOR_SIZE);
    uint8_t *ents = kzalloc(32 * SECTOR_SIZE);      /* 128 entries x 128 bytes */

    /* protective MBR */
    uint8_t *e = sec + 446;
    e[1] = 0x00; e[2] = 0x02; e[3] = 0x00;
    e[4] = 0xEE;
    e[5] = 0xFF; e[6] = 0xFF; e[7] = 0xFF;
    *(uint32_t *)(e + 8) = 1;
    *(uint32_t *)(e + 12) = (uint32_t)MIN(total - 1, 0xFFFFFFFFull);
    sec[510] = 0x55; sec[511] = 0xAA;
    int r = blk_write(disk, 0, 1, sec);

    /* the one partition entry */
    memcpy(ents, esp ? guid_esp : guid_basic, 16);
    random_guid(ents + 16);
    *(uint64_t *)(ents + 32) = first;
    *(uint64_t *)(ents + 40) = last;
    for (int i = 0; part_name[i] && i < 36; i++) ((uint16_t *)(ents + 56))[i] = (uint8_t)part_name[i];
    uint32_t ents_crc = crc32(0, ents, 32 * SECTOR_SIZE);

    uint8_t disk_guid[16];
    random_guid(disk_guid);
    for (int copy = 0; copy < 2 && !r; copy++) {
        uint64_t my = copy ? total - 1 : 1, alt = copy ? 1 : total - 1;
        uint64_t ents_lba = copy ? total - 33 : 2;
        memset(sec, 0, SECTOR_SIZE);
        memcpy(sec, "EFI PART", 8);
        *(uint32_t *)(sec + 8) = 0x00010000;
        *(uint32_t *)(sec + 12) = 92;
        *(uint64_t *)(sec + 24) = my;
        *(uint64_t *)(sec + 32) = alt;
        *(uint64_t *)(sec + 40) = 34;
        *(uint64_t *)(sec + 48) = last_usable;
        memcpy(sec + 56, disk_guid, 16);
        *(uint64_t *)(sec + 72) = ents_lba;
        *(uint32_t *)(sec + 80) = 128;
        *(uint32_t *)(sec + 84) = 128;
        *(uint32_t *)(sec + 88) = ents_crc;
        *(uint32_t *)(sec + 16) = crc32(0, sec, 92);
        r = blk_write(disk, ents_lba, 32, ents);
        if (!r) r = blk_write(disk, my, 1, sec);
    }
    if (!r) r = blk_flush(disk);
    kfree(sec);
    kfree(ents);
    if (!r) blk_rescan(disk);
    return r;
}

/* mark a partition as an EFI System Partition so every firmware boots from it */
int part_make_bootable(struct blockdev *part)
{
    struct blockdev *disk = part->parent;
    if (!disk) return 0;                            /* whole-disk file system: nothing to mark */
    if (!part->type_guid[0] && !part->type_guid[1] && part->mbr_type) {
        uint8_t *sec = kmalloc(SECTOR_SIZE);
        int r = blk_read(disk, 0, 1, sec);
        if (!r && part->partno >= 1 && part->partno <= 4) {
            sec[446 + (part->partno - 1) * 16 + 4] = 0xEF;
            r = blk_write(disk, 0, 1, sec);
        }
        kfree(sec);
        if (!r) part->mbr_type = 0xEF;
        return r;
    }
    if (!memcmp(part->type_guid, guid_esp, 16)) return 0;
    uint8_t *hdr = kmalloc(SECTOR_SIZE);
    int r = blk_read(disk, 1, 1, hdr);
    if (r || memcmp(hdr, "EFI PART", 8)) { kfree(hdr); return E_INVAL; }
    uint32_t nents = *(uint32_t *)(hdr + 80), esz = *(uint32_t *)(hdr + 84);
    uint64_t alt = *(uint64_t *)(hdr + 32);
    uint32_t secs = (nents * esz + SECTOR_SIZE - 1) / SECTOR_SIZE;
    uint8_t *ents = kmalloc((size_t)secs * SECTOR_SIZE);
    for (int copy = 0; copy < 2 && !r; copy++) {
        uint64_t hl = copy ? alt : 1;
        r = blk_read(disk, hl, 1, hdr);
        if (r || memcmp(hdr, "EFI PART", 8)) { r = copy ? 0 : E_INVAL; break; }   /* no backup: fine */
        uint64_t el = *(uint64_t *)(hdr + 72);
        r = blk_read(disk, el, secs, ents);
        if (r) break;
        memcpy(ents + (size_t)(part->partno - 1) * esz, guid_esp, 16);
        *(uint32_t *)(hdr + 88) = crc32(0, ents, (size_t)nents * esz);
        *(uint32_t *)(hdr + 16) = 0;
        *(uint32_t *)(hdr + 16) = crc32(0, hdr, *(uint32_t *)(hdr + 12));
        r = blk_write(disk, el, secs, ents);
        if (!r) r = blk_write(disk, hl, 1, hdr);
    }
    kfree(ents);
    kfree(hdr);
    if (!r) {
        memcpy(part->type_guid, guid_esp, 16);
        blk_flush(disk);
    }
    return r;
}

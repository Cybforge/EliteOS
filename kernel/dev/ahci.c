/*
 * AHCI (SATA) host controller driver.
 *
 * One command slot per port, polled completion, 48-bit LBA DMA through a
 * physically contiguous bounce buffer. Registers every SATA disk as a
 * block device (sda, sdb, ...).
 */
#include <block.h>
#include <dev.h>
#include <mm.h>
#include <x86.h>
#include <sched.h>
#include <vfs.h>

/* HBA registers */
#define HBA_CAP  0x00
#define HBA_GHC  0x04
#define HBA_IS   0x08
#define HBA_PI   0x0C
#define HBA_VS   0x10
#define HBA_CAP2 0x24
#define HBA_BOHC 0x28

/* port registers (base 0x100 + port * 0x80) */
#define PX_CLB   0x00
#define PX_CLBU  0x04
#define PX_FB    0x08
#define PX_FBU   0x0C
#define PX_IS    0x10
#define PX_IE    0x14
#define PX_CMD   0x18
#define PX_TFD   0x20
#define PX_SIG   0x24
#define PX_SSTS  0x28
#define PX_SCTL  0x2C
#define PX_SERR  0x30
#define PX_SACT  0x34
#define PX_CI    0x38

#define CMD_ST  (1u << 0)
#define CMD_SUD (1u << 1)
#define CMD_POD (1u << 2)
#define CMD_FRE (1u << 4)
#define CMD_FR  (1u << 14)
#define CMD_CR  (1u << 15)

#define TFD_ERR (1u << 0)
#define TFD_DRQ (1u << 3)
#define TFD_BSY (1u << 7)

#define SIG_ATA   0x00000101
#define SIG_ATAPI 0xEB140101

#define ATA_IDENTIFY      0xEC
#define ATA_READ_DMA_EXT  0x25
#define ATA_WRITE_DMA_EXT 0x35
#define ATA_FLUSH_EXT     0xEA

#define BOUNCE_SECTORS 128          /* 64 KiB per command */

struct cmd_header {
    uint16_t flags;                 /* CFL (dwords) | W bit 6 | ... */
    uint16_t prdtl;
    volatile uint32_t prdbc;
    uint64_t ctba;
    uint32_t rsv[4];
} PACKED;

struct prd {
    uint64_t dba;
    uint32_t rsv;
    uint32_t dbc;                   /* byte count - 1, bit 31 = interrupt */
} PACKED;

struct cmd_table {
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t rsv[48];
    struct prd prdt[1];
} PACKED;

struct ahci_port {
    struct blockdev dev;
    volatile uint8_t *regs;
    struct cmd_header *clist;
    struct cmd_table *table;
    uint8_t *bounce;
    uint64_t bounce_phys;
    mutex_t lock;
    bool lba48;
};

static volatile uint8_t *hba;
static int ndisks;

static uint32_t hr(uint32_t r) { return *(volatile uint32_t *)(hba + r); }
static void hw(uint32_t r, uint32_t v) { *(volatile uint32_t *)(hba + r) = v; }
static uint32_t pr(struct ahci_port *p, uint32_t r) { return *(volatile uint32_t *)(p->regs + r); }
static void pw(struct ahci_port *p, uint32_t r, uint32_t v) { *(volatile uint32_t *)(p->regs + r) = v; }

static bool wait_clear(struct ahci_port *p, uint32_t reg, uint32_t mask, int ms)
{
    uint64_t end = uptime_ms() + (uint64_t)ms;
    while (pr(p, reg) & mask) {
        if (uptime_ms() > end) return false;
        cpu_relax();
    }
    return true;
}

static void port_stop(struct ahci_port *p)
{
    pw(p, PX_CMD, pr(p, PX_CMD) & ~CMD_ST);
    wait_clear(p, PX_CMD, CMD_CR, 500);
    pw(p, PX_CMD, pr(p, PX_CMD) & ~CMD_FRE);
    wait_clear(p, PX_CMD, CMD_FR, 500);
}

static void port_start(struct ahci_port *p)
{
    wait_clear(p, PX_CMD, CMD_CR, 500);
    pw(p, PX_CMD, pr(p, PX_CMD) | CMD_FRE);
    pw(p, PX_CMD, pr(p, PX_CMD) | CMD_ST);
}

/* issue one command in slot 0; data goes through the bounce buffer */
static int issue(struct ahci_port *p, uint8_t cmd, uint64_t lba, uint32_t count, uint32_t bytes, bool write)
{
    if (!wait_clear(p, PX_TFD, TFD_BSY | TFD_DRQ, 2000)) return E_IO;
    struct cmd_header *h = &p->clist[0];
    h->flags = (uint16_t)(5 | (write ? 1u << 6 : 0));    /* CFL = 5 dwords */
    h->prdtl = bytes ? 1 : 0;
    h->prdbc = 0;
    struct cmd_table *t = p->table;
    memset(t->cfis, 0, sizeof(t->cfis));
    t->cfis[0] = 0x27;                  /* H2D register FIS */
    t->cfis[1] = 0x80;                  /* command */
    t->cfis[2] = cmd;
    t->cfis[4] = (uint8_t)lba;
    t->cfis[5] = (uint8_t)(lba >> 8);
    t->cfis[6] = (uint8_t)(lba >> 16);
    t->cfis[7] = 1 << 6;                /* LBA mode */
    t->cfis[8] = (uint8_t)(lba >> 24);
    t->cfis[9] = (uint8_t)(lba >> 32);
    t->cfis[10] = (uint8_t)(lba >> 40);
    t->cfis[12] = (uint8_t)count;
    t->cfis[13] = (uint8_t)(count >> 8);
    if (bytes) {
        t->prdt[0].dba = p->bounce_phys;
        t->prdt[0].rsv = 0;
        t->prdt[0].dbc = bytes - 1;
    }
    pw(p, PX_IS, 0xFFFFFFFF);
    pw(p, PX_SERR, 0xFFFFFFFF);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    pw(p, PX_CI, 1);
    uint64_t end = uptime_ms() + 10000;
    for (uint64_t spins = 0;; spins++) {
        if (!(pr(p, PX_CI) & 1)) break;
        if (pr(p, PX_IS) & (1u << 30)) break;           /* task file error */
        if (uptime_ms() > end) {
            klog("ahci: %s: command %#x timed out", p->dev.name, cmd);
            port_stop(p);
            port_start(p);
            return E_IO;
        }
        if (spins > 2000) sched_yield(); else cpu_relax();
    }
    if ((pr(p, PX_IS) & (1u << 30)) || (pr(p, PX_TFD) & TFD_ERR)) {
        klog("ahci: %s: command %#x failed, tfd %#x", p->dev.name, cmd, pr(p, PX_TFD));
        port_stop(p);
        pw(p, PX_SERR, 0xFFFFFFFF);
        pw(p, PX_IS, 0xFFFFFFFF);
        port_start(p);
        return E_IO;
    }
    return 0;
}

static int ahci_rw(struct blockdev *d, uint64_t lba, uint32_t count, void *buf, bool write)
{
    struct ahci_port *p = d->priv;
    uint8_t *b = buf;
    int r = 0;
    mutex_lock(&p->lock);
    while (count && !r) {
        uint32_t n = MIN(count, (uint32_t)BOUNCE_SECTORS);
        if (write) memcpy(p->bounce, b, (size_t)n * SECTOR_SIZE);
        r = issue(p, write ? ATA_WRITE_DMA_EXT : ATA_READ_DMA_EXT, lba, n, n * SECTOR_SIZE, write);
        if (!r && !write) memcpy(b, p->bounce, (size_t)n * SECTOR_SIZE);
        lba += n;
        count -= n;
        b += (size_t)n * SECTOR_SIZE;
    }
    mutex_unlock(&p->lock);
    return r;
}

static int ahci_read(struct blockdev *d, uint64_t lba, uint32_t count, void *buf)
{
    return ahci_rw(d, lba, count, buf, false);
}

static int ahci_write(struct blockdev *d, uint64_t lba, uint32_t count, const void *buf)
{
    return ahci_rw(d, lba, count, (void *)buf, true);
}

static int ahci_flush(struct blockdev *d)
{
    struct ahci_port *p = d->priv;
    mutex_lock(&p->lock);
    int r = issue(p, ATA_FLUSH_EXT, 0, 0, 0, false);
    mutex_unlock(&p->lock);
    return r;
}

static void ata_string(const uint16_t *id, int first, int words, char *out, size_t n)
{
    size_t o = 0;
    for (int i = 0; i < words && o + 2 < n; i++) {
        out[o++] = (char)(id[first + i] >> 8);
        out[o++] = (char)(id[first + i] & 0xFF);
    }
    out[o] = 0;
    while (o && out[o - 1] == ' ') out[--o] = 0;
    char *s = out;
    while (*s == ' ') s++;
    if (s != out) memmove(out, s, strlen(s) + 1);
}

static void port_init(int idx, volatile uint8_t *regs)
{
    struct ahci_port *p = kzalloc(sizeof(*p));
    p->regs = regs;
    p->lock = (mutex_t)MUTEX_INIT("ahci");
    port_stop(p);

    /* command list (1 KiB), received FIS (256 B) and one command table share a page */
    uint64_t page = pmm_alloc_below(0x100000000ull);
    uint64_t bounce = pmm_alloc_contig(BOUNCE_SECTORS * SECTOR_SIZE / PAGE_SIZE);
    if (!page || !bounce) { kfree(p); return; }
    uint8_t *v = P2V(page);
    p->clist = (struct cmd_header *)v;
    p->table = (struct cmd_table *)(v + 2048);
    p->bounce = P2V(bounce);
    p->bounce_phys = bounce;
    pw(p, PX_CLB, (uint32_t)page);
    pw(p, PX_CLBU, (uint32_t)(page >> 32));
    pw(p, PX_FB, (uint32_t)(page + 1024));
    pw(p, PX_FBU, (uint32_t)((page + 1024) >> 32));
    p->clist[0].ctba = page + 2048;
    pw(p, PX_SERR, 0xFFFFFFFF);
    pw(p, PX_IS, 0xFFFFFFFF);
    pw(p, PX_IE, 0);
    pw(p, PX_CMD, pr(p, PX_CMD) | CMD_SUD | CMD_POD);
    port_start(p);

    if (issue(p, ATA_IDENTIFY, 0, 0, 512, false)) {
        klog("ahci: port %d: IDENTIFY failed", idx);
        return;
    }
    uint16_t id[256];
    memcpy(id, p->bounce, 512);
    p->lba48 = id[83] & (1u << 10);
    uint64_t sectors = 0;
    if (p->lba48) memcpy(&sectors, &id[100], 8);
    else memcpy(&sectors, &id[60], 4);
    if (!sectors) return;

    struct blockdev *d = &p->dev;
    snprintf(d->name, sizeof(d->name), "sd%c", 'a' + ndisks++);
    ata_string(id, 27, 20, d->model, sizeof(d->model));
    if (!d->model[0]) strlcpy(d->model, "SATA disk", sizeof(d->model));
    d->sectors = sectors;
    d->read = ahci_read;
    d->write = ahci_write;
    d->flush = ahci_flush;
    d->priv = p;
    blk_register(d);
}

void ahci_init(void)
{
    for (int i = 0; i < pci_count(); i++) {
        struct pci_dev *pd = pci_get(i);
        if (pd->cls != 0x01 || pd->subcls != 0x06 || pd->progif != 0x01) continue;
        uint64_t abar = pci_bar_addr(pd, 5);
        if (!abar) continue;
        pci_enable_busmaster(pd);
        /* memory space enable */
        pci_write16(pd->bus, pd->dev, pd->fn, 0x04, (uint16_t)(pci_read16(pd->bus, pd->dev, pd->fn, 0x04) | 0x06));
        hba = vmm_map_mmio(abar, 0x1100);

        /* take the controller from the firmware if it supports hand-off */
        if ((hr(HBA_CAP2) & 1) && (hr(HBA_BOHC) & 1)) {
            hw(HBA_BOHC, hr(HBA_BOHC) | (1u << 1));
            for (int t = 0; t < 250 && (hr(HBA_BOHC) & 1); t++) mdelay(1);
        }
        hw(HBA_GHC, hr(HBA_GHC) | (1u << 31));          /* AHCI enable */
        hw(HBA_GHC, hr(HBA_GHC) & ~(1u << 1));          /* no interrupts, we poll */
        uint32_t pi = hr(HBA_PI);
        uint32_t vs = hr(HBA_VS);
        klog("ahci: controller %02x:%02x.%x, version %x.%x, ports %#x", pd->bus, pd->dev, pd->fn,
             vs >> 16, vs & 0xFFFF, pi);
        for (int port = 0; port < 32; port++) {
            if (!(pi & (1u << port))) continue;
            volatile uint8_t *regs = hba + 0x100 + port * 0x80;
            uint32_t ssts = *(volatile uint32_t *)(regs + PX_SSTS);
            if ((ssts & 0xF) != 3) continue;             /* no device / no phy */
            uint32_t sig = *(volatile uint32_t *)(regs + PX_SIG);
            if (sig == SIG_ATAPI) { klog("ahci: port %d: optical drive (not supported)", port); continue; }
            if (sig != SIG_ATA && sig != 0xFFFFFFFF) continue;
            port_init(port, regs);
        }
    }
}

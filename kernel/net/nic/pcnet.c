/*
 * AMD PCnet family (Am79C970/970A/971/972/973/975/978): 32-bit software
 * style 2 descriptor rings, programmed through the RAP/RDP/BDP ports in
 * 32-bit I/O mode.
 */
#include <kernel.h>
#include <dev.h>
#include <mm.h>
#include <spinlock.h>
#include <x86.h>
#include "../netdev.h"

#define RDP   0x10
#define RAP   0x14
#define RESET 0x18
#define BDP   0x1C

#define NRX 128
#define NTX 32
#define BUFSZ 1544

#define OWN  (1u << 31)
#define ERR  (1u << 30)
#define STP  (1u << 25)
#define ENP  (1u << 24)

struct desc { uint32_t addr, flags, misc, user; };

struct PACKED init_block {
    uint16_t mode;
    uint8_t rlen, tlen;
    uint8_t padr[6];
    uint16_t reserved;
    uint8_t ladrf[8];
    uint32_t rdra, tdra;
};

struct pcnet {
    struct netdev nd;
    uint16_t io;
    struct desc *rx, *tx;
    uint8_t *rxbuf, *txbuf;
    unsigned rx_cur, tx_cur;
    spinlock_t lock;
};

static uint32_t csr_read(struct pcnet *c, uint32_t n) { outl((uint16_t)(c->io + RAP), n); return inl((uint16_t)(c->io + RDP)); }
static void csr_write(struct pcnet *c, uint32_t n, uint32_t v) { outl((uint16_t)(c->io + RAP), n); outl((uint16_t)(c->io + RDP), v); }
static uint32_t bcr_read(struct pcnet *c, uint32_t n) { outl((uint16_t)(c->io + RAP), n); return inl((uint16_t)(c->io + BDP)); }
static void bcr_write(struct pcnet *c, uint32_t n, uint32_t v) { outl((uint16_t)(c->io + RAP), n); outl((uint16_t)(c->io + BDP), v); }

static uint32_t bcnt(size_t len) { return (uint32_t)(-(int32_t)len) & 0x0FFF; }

static bool pcnet_send(struct netdev *nd, const void *data, size_t len)
{
    struct pcnet *c = nd->priv;
    if (len > BUFSZ) return false;
    spin_lock(&c->lock);
    struct desc *d = &c->tx[c->tx_cur];
    for (int i = 0; i < 100000 && (((volatile struct desc *)d)->flags & OWN); i++) cpu_relax();
    if (d->flags & OWN) { spin_unlock(&c->lock); return false; }
    memcpy(c->txbuf + (size_t)c->tx_cur * BUFSZ, data, len);
    d->misc = 0;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    d->flags = OWN | STP | ENP | 0xF000 | bcnt(len);
    c->tx_cur = (c->tx_cur + 1) % NTX;
    csr_write(c, 0, 0x0008);                        /* TDMD: look at the ring now */
    spin_unlock(&c->lock);
    return true;
}

static int pcnet_poll(struct netdev *nd, void *buf, size_t cap)
{
    struct pcnet *c = nd->priv;
    for (;;) {
        struct desc *d = &c->rx[c->rx_cur];
        uint32_t f = ((volatile struct desc *)d)->flags;
        if (f & OWN) return 0;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        int len = 0;
        if (!(f & ERR) && (f & STP) && (f & ENP)) {
            int n = (int)(d->misc & 0x0FFF) - 4;      /* message length includes the CRC */
            if (n > 0) {
                len = MIN(n, (int)cap);
                memcpy(buf, c->rxbuf + (size_t)c->rx_cur * BUFSZ, (size_t)len);
            }
        }
        d->misc = 0;
        __atomic_thread_fence(__ATOMIC_RELEASE);
        d->flags = OWN | 0xF000 | bcnt(BUFSZ);
        c->rx_cur = (c->rx_cur + 1) % NRX;
        if (len) return len;
    }
}

static bool pcnet_link(struct netdev *nd)
{
    struct pcnet *c = nd->priv;
    spin_lock(&c->lock);
    bool up = bcr_read(c, 4) & 0x8000;              /* LED0 programmed as link status */
    spin_unlock(&c->lock);
    return up;
}

static const char *chip_name(uint32_t part)
{
    switch (part) {
    case 0x2420: return "AMD PCnet-PCI Am79C970";
    case 0x2621: return "AMD PCnet-PCI II Am79C970A";
    case 0x2623: return "AMD PCnet-FAST Am79C971";
    case 0x2624: return "AMD PCnet-FAST+ Am79C972";
    case 0x2625: return "AMD PCnet-FAST III Am79C973";
    case 0x2626: return "AMD PCnet-Home Am79C978";
    case 0x2627: return "AMD PCnet-FAST III Am79C975";
    default: return NULL;
    }
}

static struct netdev *pcnet_attach(struct pci_dev *p, const struct nic_id *id)
{
    if (!pci_bar_is_io(p, 0)) return NULL;
    struct pcnet *c = kzalloc(sizeof(*c));
    c->io = (uint16_t)pci_bar_addr(p, 0);
    c->lock = (spinlock_t)SPINLOCK_INIT("pcnet");
    pci_enable_busmaster(p);

    /* reset (reading RESET in both word and dword mode); the address PROM
       answers byte reads only in 16-bit mode, so read it before entering 32-bit I/O */
    inl((uint16_t)(c->io + RESET));
    inw((uint16_t)(c->io + 0x14));
    udelay(10);
    for (int i = 0; i < 6; i++) c->nd.mac[i] = inb((uint16_t)(c->io + i));
    outl((uint16_t)(c->io + RDP), 0);

    csr_write(c, 0, 0x0004);                        /* STOP */
    bcr_write(c, 20, (bcr_read(c, 20) & ~0xFFu) | 2);   /* software style 2: 32-bit PCnet-PCI */
    bcr_write(c, 2, bcr_read(c, 2) | 0x0002);       /* ASEL: automatic media selection */
    bcr_write(c, 4, 0x0040);                        /* LED0 = link status only */
    uint32_t part = ((csr_read(c, 89) << 16 | (csr_read(c, 88) & 0xFFFF)) >> 12) & 0xFFFF;

    uint64_t rx_phys, tx_phys, ib_phys;
    c->rx = dma_alloc(NRX * sizeof(struct desc), &rx_phys);
    c->tx = dma_alloc(NTX * sizeof(struct desc), &tx_phys);
    c->rxbuf = dma_alloc(NRX * BUFSZ, NULL);
    c->txbuf = dma_alloc(NTX * BUFSZ, NULL);
    struct init_block *ib = dma_alloc(sizeof(*ib), &ib_phys);
    if (!c->rx || !c->tx || !c->rxbuf || !c->txbuf || !ib) return NULL;
    for (int i = 0; i < NRX; i++) {
        c->rx[i].addr = dma32(c->rxbuf + (size_t)i * BUFSZ);
        c->rx[i].flags = OWN | 0xF000 | bcnt(BUFSZ);
    }
    for (int i = 0; i < NTX; i++) c->tx[i].addr = dma32(c->txbuf + (size_t)i * BUFSZ);

    ib->mode = 0;
    ib->rlen = 7 << 4;                              /* 2^7 = 128 descriptors */
    ib->tlen = 5 << 4;                              /* 2^5 = 32 */
    memcpy(ib->padr, c->nd.mac, 6);
    memset(ib->ladrf, 0xFF, 8);                     /* all multicast */
    ib->rdra = (uint32_t)rx_phys;
    ib->tdra = (uint32_t)tx_phys;
    csr_write(c, 1, (uint32_t)ib_phys & 0xFFFF);
    csr_write(c, 2, (uint32_t)ib_phys >> 16);
    csr_write(c, 3, 0x5F00);                        /* mask all interrupt sources */
    csr_write(c, 4, csr_read(c, 4) | 0x0800);      /* pad short frames */
    csr_write(c, 0, 0x0001);                        /* INIT */
    for (int i = 0; i < 1000 && !(csr_read(c, 0) & 0x0100); i++) udelay(10);
    if (!(csr_read(c, 0) & 0x0100)) klog("pcnet: initialization did not complete");
    csr_write(c, 0, 0x0100 | 0x0002);               /* clear IDON, STRT */

    const char *chip = chip_name(part);
    strlcpy(c->nd.name, chip ? chip : id->model, sizeof(c->nd.name));
    c->nd.send = pcnet_send;
    c->nd.poll = pcnet_poll;
    c->nd.link = pcnet_link;
    c->nd.priv = c;
    return &c->nd;
}

static const struct nic_id ids[] = {
    { 0x1022, 0x2000, "AMD PCnet-PCI II / PCnet-FAST (Am79C970A-Am79C975)" },
    { 0x1022, 0x2001, "AMD PCnet-Home (Am79C978)" },
    { 0 },
};

const struct nic_driver drv_pcnet = { "pcnet", "AMD PCnet (Am79C970, 970A, 971, 972, 973, 975, 978)", ids, pcnet_attach };

/*
 * NE2000-compatible PCI cards (RTL8029, Winbond 89C940, VIA 86C926 ...):
 * the National DP8390 register set. Frames live in the card's own 16 KiB
 * buffer and are copied in and out with "remote DMA" through a data port.
 */
#include <kernel.h>
#include <dev.h>
#include <mm.h>
#include <spinlock.h>
#include <x86.h>
#include "../netdev.h"

#define CR     0x00
#define PSTART 0x01
#define PSTOP  0x02
#define BNRY   0x03
#define TPSR   0x04
#define TBCR0  0x05
#define TBCR1  0x06
#define ISR    0x07
#define RSAR0  0x08
#define RSAR1  0x09
#define RBCR0  0x0A
#define RBCR1  0x0B
#define RCR    0x0C
#define TCR    0x0D
#define DCR    0x0E
#define IMR    0x0F
#define DATA   0x10
#define RESETP 0x1F
#define PAR0   0x01     /* page 1 */
#define CURR   0x07
#define MAR0   0x08

#define CR_STP 0x01
#define CR_STA 0x02
#define CR_TXP 0x04
#define CR_RD_READ  0x08
#define CR_RD_WRITE 0x10
#define CR_RD_ABORT 0x20
#define CR_PAGE1 0x40

#define ISR_PRX 0x01
#define ISR_PTX 0x02
#define ISR_TXE 0x08
#define ISR_OVW 0x10
#define ISR_RDC 0x40
#define ISR_RST 0x80

#define TX_PAGE 0x40
#define RX_START 0x46
#define RX_STOP 0x80

struct ne2k {
    struct netdev nd;
    uint16_t io;
    uint8_t next;           /* next receive page */
    spinlock_t lock;        /* one remote DMA at a time */
};

static void reg(struct ne2k *n, int r, uint8_t v) { outb((uint16_t)(n->io + r), v); }
static uint8_t get(struct ne2k *n, int r) { return inb((uint16_t)(n->io + r)); }

static bool wait_isr(struct ne2k *n, uint8_t bit)
{
    for (int i = 0; i < 100000; i++) {
        if (get(n, ISR) & bit) { reg(n, ISR, bit); return true; }
        cpu_relax();
    }
    return false;
}

static void remote_read(struct ne2k *n, uint16_t addr, void *dst, size_t len)
{
    size_t even = (len + 1) & ~1ul;
    reg(n, CR, CR_STA | CR_RD_ABORT);
    reg(n, RBCR0, (uint8_t)even);
    reg(n, RBCR1, (uint8_t)(even >> 8));
    reg(n, RSAR0, (uint8_t)addr);
    reg(n, RSAR1, (uint8_t)(addr >> 8));
    reg(n, CR, CR_STA | CR_RD_READ);
    uint8_t *d = dst;
    for (size_t i = 0; i < even; i += 2) {
        uint16_t w = inw((uint16_t)(n->io + DATA));
        d[i] = (uint8_t)w;
        if (i + 1 < len) d[i + 1] = (uint8_t)(w >> 8);
    }
    wait_isr(n, ISR_RDC);
}

static bool ne2k_send(struct netdev *nd, const void *data, size_t len)
{
    struct ne2k *n = nd->priv;
    if (len > 1514) return false;
    if (len < 60) len = 60;                   /* the stack pads frames it builds itself */
    size_t even = (len + 1) & ~1ul;
    spin_lock(&n->lock);
    /* wait for the previous frame to leave the card */
    for (int i = 0; i < 100000 && (get(n, CR) & CR_TXP); i++) cpu_relax();
    reg(n, CR, CR_STA | CR_RD_ABORT);
    reg(n, ISR, ISR_RDC);
    reg(n, RBCR0, (uint8_t)even);
    reg(n, RBCR1, (uint8_t)(even >> 8));
    reg(n, RSAR0, 0);
    reg(n, RSAR1, TX_PAGE);
    reg(n, CR, CR_STA | CR_RD_WRITE);
    const uint8_t *s = data;
    for (size_t i = 0; i < even; i += 2) {
        uint16_t w = s[i] | (uint16_t)((i + 1 < len ? s[i + 1] : 0) << 8);
        outw((uint16_t)(n->io + DATA), w);
    }
    wait_isr(n, ISR_RDC);
    reg(n, TPSR, TX_PAGE);
    reg(n, TBCR0, (uint8_t)len);
    reg(n, TBCR1, (uint8_t)(len >> 8));
    reg(n, CR, CR_STA | CR_TXP | CR_RD_ABORT);
    spin_unlock(&n->lock);
    return true;
}

static void rx_reset(struct ne2k *n)
{
    /* buffer overflow: the DP8390 wants a stop/restart */
    reg(n, CR, CR_STP | CR_RD_ABORT);
    mdelay(2);
    reg(n, RBCR0, 0);
    reg(n, RBCR1, 0);
    reg(n, TCR, 0x02);
    reg(n, CR, CR_STA | CR_RD_ABORT);
    reg(n, BNRY, RX_START);
    reg(n, CR, CR_PAGE1 | CR_STA | CR_RD_ABORT);
    reg(n, CURR, RX_START + 1);
    reg(n, CR, CR_STA | CR_RD_ABORT);
    n->next = RX_START + 1;
    reg(n, ISR, ISR_OVW);
    reg(n, TCR, 0);
}

static int ne2k_poll(struct netdev *nd, void *buf, size_t cap)
{
    struct ne2k *n = nd->priv;
    int got = 0;
    spin_lock(&n->lock);
    if (get(n, ISR) & ISR_OVW) { rx_reset(n); spin_unlock(&n->lock); return 0; }
    reg(n, CR, CR_PAGE1 | CR_STA | CR_RD_ABORT);
    uint8_t curr = get(n, CURR);
    reg(n, CR, CR_STA | CR_RD_ABORT);
    while (n->next != curr && !got) {
        uint8_t hdr[4];
        remote_read(n, (uint16_t)(n->next << 8), hdr, 4);
        uint8_t next = hdr[1];
        size_t len = (size_t)(hdr[2] | hdr[3] << 8);
        if (next < RX_START || next >= RX_STOP || len < 4 + 14 || len > 4 + 1518) {
            rx_reset(n);
            break;
        }
        if (hdr[0] & 1) {
            size_t fl = MIN(len - 4, cap);             /* minus the header; a trailing CRC is harmless */
            /* remote DMA wraps from PSTOP back to PSTART by itself */
            remote_read(n, (uint16_t)((n->next << 8) + 4), buf, fl);
            got = (int)fl;
        }
        n->next = next;
        reg(n, BNRY, (uint8_t)(next == RX_START ? RX_STOP - 1 : next - 1));
    }
    reg(n, ISR, ISR_PRX | ISR_PTX | ISR_TXE);
    spin_unlock(&n->lock);
    return got;
}

static bool ne2k_link(struct netdev *nd)
{
    UNUSED(nd);
    return true;            /* the DP8390 has no link status */
}

static struct netdev *ne2k_attach(struct pci_dev *p, const struct nic_id *id)
{
    if (!pci_bar_is_io(p, 0)) return NULL;
    struct ne2k *n = kzalloc(sizeof(*n));
    n->io = (uint16_t)pci_bar_addr(p, 0);
    n->lock = (spinlock_t)SPINLOCK_INIT("ne2k");
    pci_enable_busmaster(p);

    reg(n, RESETP, get(n, RESETP));
    for (int i = 0; i < 1000 && !(get(n, ISR) & ISR_RST); i++) udelay(10);
    reg(n, ISR, 0xFF);
    reg(n, CR, CR_STP | CR_RD_ABORT);
    reg(n, DCR, 0x49);                 /* word transfers, normal operation, FIFO threshold 8 bytes */
    reg(n, RBCR0, 0);
    reg(n, RBCR1, 0);
    reg(n, RCR, 0x20);                 /* monitor mode while setting up */
    reg(n, TCR, 0x02);                 /* internal loopback */
    reg(n, ISR, 0xFF);
    reg(n, IMR, 0);

    /* the station address PROM: 16 words, each MAC byte doubled */
    uint8_t prom[32];
    remote_read(n, 0, prom, 32);
    for (int i = 0; i < 6; i++) n->nd.mac[i] = prom[i * 2];

    reg(n, PSTART, RX_START);
    reg(n, PSTOP, RX_STOP);
    reg(n, BNRY, RX_START);
    reg(n, TPSR, TX_PAGE);
    reg(n, CR, CR_PAGE1 | CR_STP | CR_RD_ABORT);
    for (int i = 0; i < 6; i++) reg(n, PAR0 + i, n->nd.mac[i]);
    for (int i = 0; i < 8; i++) reg(n, MAR0 + i, 0xFF);
    reg(n, CURR, RX_START + 1);
    n->next = RX_START + 1;
    reg(n, CR, CR_STA | CR_RD_ABORT);
    reg(n, ISR, 0xFF);
    reg(n, TCR, 0x00);
    reg(n, RCR, 0x04 | 0x08);          /* accept broadcast and multicast */

    snprintf(n->nd.name, sizeof(n->nd.name), "%s", id->model);
    n->nd.send = ne2k_send;
    n->nd.poll = ne2k_poll;
    n->nd.link = ne2k_link;
    n->nd.priv = n;
    n->nd.rx_window = 8192;            /* the on-card ring holds only ~9 full frames */
    return &n->nd;
}

static const struct nic_id ids[] = {
    { 0x10EC, 0x8029, "Realtek RTL8029 (NE2000 PCI)" },
    { 0x1050, 0x0940, "Winbond W89C940 (NE2000 PCI)" },
    { 0x1050, 0x5A5A, "Winbond W89C940F (NE2000 PCI)" },
    { 0x11F6, 0x1401, "Compex RL2000 (NE2000 PCI)" },
    { 0x8E2E, 0x3000, "KTI ET32P2 (NE2000 PCI)" },
    { 0x4A14, 0x5000, "NetVin NV5000SC (NE2000 PCI)" },
    { 0x1106, 0x0926, "VIA VT86C926 (NE2000 PCI)" },
    { 0x10BD, 0x0E34, "SureCom NE34 (NE2000 PCI)" },
    { 0x12C3, 0x0058, "Holtek HT80232 (NE2000 PCI)" },
    { 0x12C3, 0x5598, "Holtek HT80229 (NE2000 PCI)" },
    { 0 },
};

const struct nic_driver drv_ne2k = { "ne2k-pci", "NE2000 compatible (DP8390 register set)", ids, ne2k_attach };

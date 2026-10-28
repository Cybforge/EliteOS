/*
 * Realtek PCI/PCIe Family Controllers in "C+" descriptor mode:
 *   RTL8169/8110      "Realtek PCI GbE Family Controller"
 *   RTL8111/8168/8411 "Realtek PCIe GbE Family Controller"
 *   RTL8101/8102/8103 "Realtek PCIe FE Family Controller"
 * They share one interface: rings of 16-byte descriptors with an OWN bit,
 * the end of each ring marked by EOR, a transmit poll register and a PHY
 * behind the PHYAR register.
 *
 * No emulator models these chips, so the driver is exercised against a
 * software model of the register interface (tests/host/rtl_model.c) but has
 * not run on a real card yet. It follows the order of the manufacturer's
 * programming sequence and does no chip-specific PHY tuning; the PHY keeps
 * the auto-negotiation set-up the chip's own firmware left.
 */
#include <kernel.h>
#include <dev.h>
#include <mm.h>
#include <spinlock.h>
#include <x86.h>
#include "../netdev.h"

#define IDR0      0x00
#define MAR0      0x08
#define TNPDS     0x20
#define CHIPCMD   0x37
#define TPPOLL    0x38
#define INTRMASK  0x3C
#define INTRSTAT  0x3E
#define TXCONFIG  0x40
#define RXCONFIG  0x44
#define CFG9346   0x50
#define PHYAR     0x60
#define PHYSTATUS 0x6C
#define RXMAXSIZE 0xDA
#define CPLUSCMD  0xE0
#define RDSAR     0xE4
#define MAXTXPKT  0xEC

#define CMD_RST 0x10
#define CMD_RE  0x08
#define CMD_TE  0x04

#define OWN (1u << 31)
#define EOR (1u << 30)
#define FS  (1u << 29)
#define LS  (1u << 28)
#define RES (1u << 21)      /* receive error summary */

#define PHYSTATUS_LINK 0x02

#define NRX 128
#define NTX 64
#define BUFSZ 2048

/* per-ID flags */
#define F_PCI    (1u << 0)      /* 8169: PCI, registers in BAR1 (MMIO) */
#define F_FE     (1u << 1)      /* 10/100 only */

struct desc { uint32_t opts1, opts2; uint64_t addr; };

struct rtl {
    struct netdev nd;
    volatile uint8_t *mmio;
    uint32_t flags;
    uint32_t xid;               /* hardware version bits of TxConfig */
    struct desc *rx, *tx;
    uint8_t *rxbuf, *txbuf;
    uint64_t rx_phys, tx_phys, rxb_phys, txb_phys;
    unsigned rx_cur, tx_cur;
    spinlock_t lock;
    int tx_stalls;
};

static bool phy_ready(struct rtl *r, bool want_set)
{
    for (int i = 0; i < 2000; i++) {
        bool set = mmio_r32(r->mmio, PHYAR) & (1u << 31);
        if (set == want_set) return true;
        udelay(20);
    }
    return false;
}

static int phy_read(struct rtl *r, int reg)
{
    mmio_w32(r->mmio, PHYAR, (uint32_t)reg << 16);
    if (!phy_ready(r, true)) return -1;
    return (int)(mmio_r32(r->mmio, PHYAR) & 0xFFFF);
}

static void phy_write(struct rtl *r, int reg, uint16_t val)
{
    mmio_w32(r->mmio, PHYAR, (1u << 31) | ((uint32_t)reg << 16) | val);
    phy_ready(r, false);
}

/* (re)build the rings and start the receiver and transmitter, in the documented order */
static void hw_start(struct rtl *r)
{
    for (int i = 0; i < NRX; i++) {
        r->rx[i].addr = r->rxb_phys + (uint64_t)i * BUFSZ;
        r->rx[i].opts2 = 0;
        r->rx[i].opts1 = OWN | BUFSZ | (i == NRX - 1 ? EOR : 0);
    }
    for (int i = 0; i < NTX; i++) {
        r->tx[i].addr = r->txb_phys + (uint64_t)i * BUFSZ;
        r->tx[i].opts2 = 0;
        r->tx[i].opts1 = i == NTX - 1 ? EOR : 0;
    }
    r->rx_cur = r->tx_cur = 0;

    mmio_w8(r->mmio, CFG9346, 0xC0);                    /* unlock the configuration registers */
    mmio_w16(r->mmio, CPLUSCMD, (uint16_t)(mmio_r16(r->mmio, CPLUSCMD) | 0x0008));   /* PCI multiple read/write */
    mmio_w8(r->mmio, CHIPCMD, CMD_TE | CMD_RE);
    mmio_w16(r->mmio, RXMAXSIZE, 1536);
    mmio_w8(r->mmio, MAXTXPKT, 0x3B);
    mmio_w32(r->mmio, TNPDS + 4, (uint32_t)(r->tx_phys >> 32));
    mmio_w32(r->mmio, TNPDS, (uint32_t)r->tx_phys);
    mmio_w32(r->mmio, RDSAR + 4, (uint32_t)(r->rx_phys >> 32));
    mmio_w32(r->mmio, RDSAR, (uint32_t)r->rx_phys);
    /* TxConfig: standard inter-frame gap, unlimited DMA bursts */
    mmio_w32(r->mmio, TXCONFIG, (mmio_r32(r->mmio, TXCONFIG) & 0xFCF00000u) | (3u << 24) | (7u << 8));
    /* RxConfig: accept our address, multicast and broadcast; no rx FIFO threshold; unlimited DMA bursts */
    mmio_w32(r->mmio, RXCONFIG, 0x0E | (7u << 8) | (7u << 13));
    mmio_w32(r->mmio, MAR0, 0xFFFFFFFF);
    mmio_w32(r->mmio, MAR0 + 4, 0xFFFFFFFF);
    mmio_w8(r->mmio, CFG9346, 0x00);                    /* lock them again */
    mmio_w16(r->mmio, INTRMASK, 0);                     /* polled: no interrupts */
    mmio_w16(r->mmio, INTRSTAT, 0xFFFF);
}

static bool r_send(struct netdev *nd, const void *data, size_t len)
{
    struct rtl *r = nd->priv;
    if (len > BUFSZ) return false;
    spin_lock(&r->lock);
    struct desc *d = &r->tx[r->tx_cur];
    for (int i = 0; i < 100000 && (((volatile struct desc *)d)->opts1 & OWN); i++) cpu_relax();
    if (d->opts1 & OWN) {
        /* the transmitter stopped taking frames: reset the chip once in a while and start over */
        if (++r->tx_stalls >= 3) {
            r->tx_stalls = 0;
            mmio_w8(r->mmio, CHIPCMD, CMD_RST);
            for (int i = 0; i < 1000 && (mmio_r8(r->mmio, CHIPCMD) & CMD_RST); i++) udelay(10);
            hw_start(r);
        }
        spin_unlock(&r->lock);
        return false;
    }
    r->tx_stalls = 0;
    memcpy(r->txbuf + (size_t)r->tx_cur * BUFSZ, data, len);
    if (len < 60) { memset(r->txbuf + (size_t)r->tx_cur * BUFSZ + len, 0, 60 - len); len = 60; }
    d->opts2 = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    d->opts1 = OWN | FS | LS | (uint32_t)len | (r->tx_cur == NTX - 1 ? EOR : 0);
    r->tx_cur = (r->tx_cur + 1) % NTX;
    mmio_w8(r->mmio, TPPOLL, 0x40);                     /* normal priority queue */
    spin_unlock(&r->lock);
    return true;
}

static int r_poll(struct netdev *nd, void *buf, size_t cap)
{
    struct rtl *r = nd->priv;
    for (;;) {
        struct desc *d = &r->rx[r->rx_cur];
        uint32_t o = ((volatile struct desc *)d)->opts1;
        if (o & OWN) return 0;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        int len = 0;
        if (!(o & RES) && (o & FS) && (o & LS)) {
            int n = (int)(o & 0x3FFF) - 4;                  /* includes the CRC */
            if (n > 0) {
                len = MIN(n, (int)cap);
                memcpy(buf, r->rxbuf + (size_t)r->rx_cur * BUFSZ, (size_t)len);
            }
        }
        d->opts2 = 0;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        d->opts1 = OWN | BUFSZ | (r->rx_cur == NRX - 1 ? EOR : 0);
        r->rx_cur = (r->rx_cur + 1) % NRX;
        mmio_w16(r->mmio, INTRSTAT, 0xFFFF);
        if (len) return len;
    }
}

static bool r_link(struct netdev *nd)
{
    struct rtl *r = nd->priv;
    return mmio_r8(r->mmio, PHYSTATUS) & PHYSTATUS_LINK;
}

/* PCIe parts power down the link on their own unless ASPM is off: the classic "link drops after a while" */
static void disable_aspm(struct pci_dev *p)
{
    uint8_t cap = pci_find_cap(p, 0x10, 0);
    if (!cap) return;
    uint16_t lc = pci_read16(p->bus, p->dev, p->fn, (uint8_t)(cap + 0x10));
    if (lc & 0x0103) pci_write16(p->bus, p->dev, p->fn, (uint8_t)(cap + 0x10), lc & (uint16_t)~0x0103);
}

static struct netdev *r_attach(struct pci_dev *p, const struct nic_id *id)
{
    /* the register window is the first memory BAR (BAR1 on the 8169, BAR2 on PCIe parts) */
    uint64_t bar = 0;
    for (int i = 0; i < 6 && !bar; i++)
        if (p->bar[i] && !pci_bar_is_io(p, i)) bar = pci_bar_addr(p, i);
    if (!bar) return NULL;
    struct rtl *r = kzalloc(sizeof(*r));
    r->lock = (spinlock_t)SPINLOCK_INIT("r8169");
    r->flags = id->flags;
    r->mmio = vmm_map_mmio(bar, 256);
    pci_enable_busmaster(p);
    disable_aspm(p);

    mmio_w16(r->mmio, INTRMASK, 0);
    mmio_w8(r->mmio, CHIPCMD, CMD_RST);
    for (int i = 0; i < 1000 && (mmio_r8(r->mmio, CHIPCMD) & CMD_RST); i++) udelay(10);
    if (mmio_r8(r->mmio, CHIPCMD) & CMD_RST) { klog("r8169: the chip did not come out of reset"); return NULL; }
    r->xid = (mmio_r32(r->mmio, TXCONFIG) >> 20) & 0x7CF;
    for (int i = 0; i < 6; i++) r->nd.mac[i] = mmio_r8(r->mmio, IDR0 + i);

    r->rx = dma_alloc(NRX * sizeof(struct desc), &r->rx_phys);      /* page aligned (the chip wants 256) */
    r->tx = dma_alloc(NTX * sizeof(struct desc), &r->tx_phys);
    r->rxbuf = dma_alloc(NRX * BUFSZ, &r->rxb_phys);
    r->txbuf = dma_alloc(NTX * BUFSZ, &r->txb_phys);
    if (!r->rx || !r->tx || !r->rxbuf || !r->txbuf) return NULL;
    hw_start(r);

    /* restart auto-negotiation; the status register tells the network thread when the link is up */
    int bmcr = phy_read(r, MII_BMCR);
    if (bmcr >= 0 && bmcr != 0xFFFF) phy_write(r, MII_BMCR, (uint16_t)((bmcr & ~(1 << 11)) | (1 << 12) | (1 << 9)));

    snprintf(r->nd.name, sizeof(r->nd.name), "%s", id->model);
    klog("r8169: %s, hardware version %03x", id->model, r->xid);
    r->nd.send = r_send;
    r->nd.poll = r_poll;
    r->nd.link = r_link;
    r->nd.priv = r;
    return &r->nd;
}

static const struct nic_id ids[] = {
    { 0x10EC, 0x8168, "Realtek PCIe GbE Family Controller (RTL8111/8168/8411)", 0 },
    { 0x10EC, 0x8161, "Realtek PCIe GbE Family Controller (RTL8111/8168 rev. B)", 0 },
    { 0x10EC, 0x8169, "Realtek PCI GbE Family Controller (RTL8169/8110)", F_PCI },
    { 0x10EC, 0x8167, "Realtek PCI GbE Family Controller (RTL8169SC/8110SC)", F_PCI },
    { 0x10EC, 0x8136, "Realtek PCIe FE Family Controller (RTL8101/8102/8103)", F_FE },
    { 0x10EC, 0x8137, "Realtek PCIe FE Family Controller (RTL8101E rev. B)", F_FE },
    { 0x1186, 0x4300, "D-Link DGE-528T (RTL8169)", F_PCI },
    { 0x1186, 0x4302, "D-Link DGE-530T rev. C (RTL8169)", F_PCI },
    { 0x1259, 0xC107, "Allied Telesyn AT-2450 (RTL8169)", F_PCI },
    { 0x16EC, 0x0116, "US Robotics USR997902 (RTL8169)", F_PCI },
    { 0x1737, 0x1032, "Linksys EG1032 v3 (RTL8169)", F_PCI },
    { 0 },
};

const struct nic_driver drv_r8169 = { "r8169", "Realtek PCI/PCIe Family Controllers (8169, 8111/8168, 8101/8102) (not yet run on real hardware)", ids, r_attach };

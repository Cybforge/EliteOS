/*
 * Intel PRO/1000 family: 8254x (e1000), 8257x/82583 and the ICH/PCH
 * LAN parts (e1000e), 82575/82576/82580/I350/I210/I211 (igb).
 *
 * All three generations understand the legacy 16-byte descriptors, so one
 * driver covers them; they differ in reset, EEPROM access and where the
 * queue registers live. Polled by the network thread, no interrupts.
 */
#include <kernel.h>
#include <dev.h>
#include <mm.h>
#include <spinlock.h>
#include <x86.h>
#include "../netdev.h"

#define REG_CTRL     0x0000
#define REG_STATUS   0x0008
#define REG_EECD     0x0010
#define REG_EERD     0x0014
#define REG_CTRL_EXT 0x0018
#define REG_MDIC     0x0020
#define REG_ICR      0x00C0
#define REG_IMC      0x00D8
#define REG_RCTL     0x0100
#define REG_TCTL     0x0400
#define REG_TIPG     0x0410
#define REG_MTA      0x5200
#define REG_RAL      0x5400
#define REG_RAH      0x5404

/* queue 0 registers: legacy location (8254x, 8257x) and igb location */
#define Q_RDBAL  0
#define Q_RDBAH  1
#define Q_RDLEN  2
#define Q_RDH    3
#define Q_RDT    4
#define Q_RXDCTL 5
#define Q_TDBAL  6
#define Q_TDBAH  7
#define Q_TDLEN  8
#define Q_TDH    9
#define Q_TDT    10
#define Q_TXDCTL 11
#define Q_SRRCTL 12
static const uint32_t qreg_legacy[] = { 0x2800, 0x2804, 0x2808, 0x2810, 0x2818, 0x2828,
                                        0x3800, 0x3804, 0x3808, 0x3810, 0x3818, 0x3828, 0 };
static const uint32_t qreg_igb[] = { 0xC000, 0xC004, 0xC008, 0xC010, 0xC018, 0xC028,
                                     0xE000, 0xE004, 0xE008, 0xE010, 0xE018, 0xE028, 0xC00C };

/* per-model flags */
#define F_EERD_SHIFT2 (1u << 0)     /* 82541 and later: EERD address at bit 2, done at bit 1 */
#define F_PCH         (1u << 1)     /* ICH/PCH LAN: firmware owns the PHY, no global reset */
#define F_NO_EERD     (1u << 2)     /* 82543/82544: no EERD register */

enum { GEN_8254X, GEN_E1000E, GEN_IGB };

#define NRX 128
#define NTX 64
#define BUFSZ 2048

struct rx_desc { uint64_t addr; uint16_t len, csum; uint8_t status, errors; uint16_t special; } PACKED;
struct tx_desc { uint64_t addr; uint16_t len; uint8_t cso, cmd, status, css; uint16_t special; } PACKED;

struct e1k {
    struct netdev nd;
    volatile uint8_t *mmio;
    int gen;
    uint32_t flags;
    const uint32_t *q;
    struct rx_desc *rx;
    struct tx_desc *tx;
    uint8_t *rxbuf, *txbuf;
    unsigned rx_cur, tx_cur;
    spinlock_t tx_lock;
};

static uint32_t rd(struct e1k *e, uint32_t r) { return mmio_r32(e->mmio, r); }
static void wr(struct e1k *e, uint32_t r, uint32_t v) { mmio_w32(e->mmio, r, v); }
static uint32_t qrd(struct e1k *e, int q) { return rd(e, e->q[q]); }
static void qwr(struct e1k *e, int q, uint32_t v) { wr(e, e->q[q], v); }

static bool e1k_send(struct netdev *nd, const void *data, size_t len)
{
    struct e1k *e = nd->priv;
    if (len > BUFSZ) return false;
    spin_lock(&e->tx_lock);
    struct tx_desc *d = &e->tx[e->tx_cur];
    /* wait for the slot to be written back (descriptor done) */
    for (int i = 0; i < 200000 && d->cmd && !(d->status & 1); i++) cpu_relax();
    if (d->cmd && !(d->status & 1)) {
        spin_unlock(&e->tx_lock);
        return false;
    }
    memcpy(e->txbuf + (size_t)e->tx_cur * BUFSZ, data, len);
    d->len = (uint16_t)len;
    d->status = 0;
    d->cso = d->css = 0;
    d->special = 0;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    d->cmd = (1 << 0) | (1 << 1) | (1 << 3);    /* EOP, IFCS, RS */
    e->tx_cur = (e->tx_cur + 1) % NTX;
    qwr(e, Q_TDT, e->tx_cur);
    spin_unlock(&e->tx_lock);
    return true;
}

/* igb: "advanced" one-buffer receive descriptors (what the 82576 family is
   normally driven with; QEMU's model knows no other) */
struct adv_rx_wb { uint32_t info, rss, status_error; uint16_t len, vlan; } PACKED;

static int igb_poll(struct e1k *e, void *buf, size_t cap)
{
    for (;;) {
        struct rx_desc *d = &e->rx[e->rx_cur];
        struct adv_rx_wb *wb = (struct adv_rx_wb *)d;
        uint32_t st = ((volatile struct adv_rx_wb *)wb)->status_error;
        if (!(st & 1)) return 0;                         /* DD */
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        int len = 0;
        if ((st & 2) && !(st & (1u << 29))) {            /* EOP, no RXE */
            len = MIN((int)wb->len, (int)cap);
            memcpy(buf, e->rxbuf + (size_t)e->rx_cur * BUFSZ, (size_t)len);
        }
        /* hand the descriptor back: the write-back replaced the buffer address */
        d->addr = V2P(e->rxbuf) + (uint64_t)e->rx_cur * BUFSZ;
        ((uint64_t *)d)[1] = 0;
        unsigned old = e->rx_cur;
        e->rx_cur = (e->rx_cur + 1) % NRX;
        qwr(e, Q_RDT, old);
        if (len) return len;
    }
}

static int e1k_poll(struct netdev *nd, void *buf, size_t cap)
{
    struct e1k *e = nd->priv;
    if (e->gen == GEN_IGB) return igb_poll(e, buf, cap);
    for (;;) {
        struct rx_desc *d = &e->rx[e->rx_cur];
        if (!(d->status & 1)) return 0;                  /* DD */
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        int len = 0;
        /* a frame split over several buffers (EOP clear) or with errors is dropped */
        if ((d->status & 2) && !d->errors) {
            len = MIN((int)d->len, (int)cap);
            memcpy(buf, e->rxbuf + (size_t)e->rx_cur * BUFSZ, (size_t)len);
        }
        d->status = 0;
        unsigned old = e->rx_cur;
        e->rx_cur = (e->rx_cur + 1) % NRX;
        qwr(e, Q_RDT, old);
        if (len) return len;
    }
}

static bool e1k_link(struct netdev *nd)
{
    struct e1k *e = nd->priv;
    return rd(e, REG_STATUS) & 2;
}

static uint16_t eeprom_read(struct e1k *e, uint8_t addr)
{
    if (e->flags & F_NO_EERD) return 0;
    uint32_t done = (e->flags & F_EERD_SHIFT2) ? (1u << 1) : (1u << 4);
    int shift = (e->flags & F_EERD_SHIFT2) ? 2 : 8;
    wr(e, REG_EERD, 1 | ((uint32_t)addr << shift));
    for (int i = 0; i < 100000; i++) {
        uint32_t v = rd(e, REG_EERD);
        if (v & done) return (uint16_t)(v >> 16);
        udelay(1);
    }
    return 0;
}

/* PHY registers through MDIC (PHY address 1 on every part we drive this way) */
static int phy_read(struct e1k *e, int reg)
{
    wr(e, REG_MDIC, ((uint32_t)reg << 16) | (1u << 21) | (2u << 26));
    for (int i = 0; i < 2000; i++) {
        uint32_t v = rd(e, REG_MDIC);
        if (v & (1u << 28)) return (v & (1u << 30)) ? -1 : (int)(v & 0xFFFF);
        udelay(5);
    }
    return -1;
}

static void phy_write(struct e1k *e, int reg, uint16_t val)
{
    wr(e, REG_MDIC, val | ((uint32_t)reg << 16) | (1u << 21) | (1u << 26));
    for (int i = 0; i < 2000 && !(rd(e, REG_MDIC) & (1u << 28)); i++) udelay(5);
}

static void read_mac(struct e1k *e)
{
    uint32_t ral = rd(e, REG_RAL), rah = rd(e, REG_RAH);
    if ((rah & (1u << 31)) && (ral || (rah & 0xFFFF))) {
        for (int i = 0; i < 4; i++) e->nd.mac[i] = (uint8_t)(ral >> (i * 8));
        e->nd.mac[4] = (uint8_t)rah;
        e->nd.mac[5] = (uint8_t)(rah >> 8);
        return;
    }
    for (int i = 0; i < 3; i++) {
        uint16_t w = eeprom_read(e, (uint8_t)i);
        e->nd.mac[i * 2] = (uint8_t)w;
        e->nd.mac[i * 2 + 1] = (uint8_t)(w >> 8);
    }
    wr(e, REG_RAL, (uint32_t)e->nd.mac[0] | ((uint32_t)e->nd.mac[1] << 8) | ((uint32_t)e->nd.mac[2] << 16) |
                   ((uint32_t)e->nd.mac[3] << 24));
    wr(e, REG_RAH, (uint32_t)e->nd.mac[4] | ((uint32_t)e->nd.mac[5] << 8) | (1u << 31));
}

static bool wait_bit(struct e1k *e, int q, uint32_t bit)
{
    for (int i = 0; i < 1000; i++) {
        if (qrd(e, q) & bit) return true;
        udelay(10);
    }
    return false;
}

static struct netdev *e1k_attach(struct pci_dev *p, const struct nic_id *id, int gen)
{
    uint64_t bar = pci_bar_addr(p, 0);
    if (!bar || pci_bar_is_io(p, 0)) return NULL;
    struct e1k *e = kzalloc(sizeof(*e));
    e->gen = gen;
    e->flags = id->flags;
    e->q = gen == GEN_IGB ? qreg_igb : qreg_legacy;
    e->tx_lock = (spinlock_t)SPINLOCK_INIT("e1000-tx");
    e->mmio = vmm_map_mmio(bar, 128 * 1024);
    pci_enable_busmaster(p);

    wr(e, REG_IMC, 0xFFFFFFFF);
    if (!(e->flags & F_PCH)) {
        /* stop DMA, then a full MAC reset; the PHY keeps its link */
        wr(e, REG_RCTL, 0);
        wr(e, REG_TCTL, (1u << 3));
        mdelay(10);
        wr(e, REG_CTRL, rd(e, REG_CTRL) | (1u << 26));
        mdelay(gen == GEN_IGB ? 20 : 10);
        for (int i = 0; i < 100 && (rd(e, REG_CTRL) & (1u << 26)); i++) mdelay(1);
        wr(e, REG_IMC, 0xFFFFFFFF);
    }
    rd(e, REG_ICR);
    /* set link up, auto speed detection; clear link reset and PHY reset */
    uint32_t ctrl = rd(e, REG_CTRL);
    ctrl |= (1u << 6) | (1u << 5);
    ctrl &= ~((1u << 3) | (1u << 31) | (1u << 7));
    wr(e, REG_CTRL, ctrl);
    if (gen == GEN_IGB || gen == GEN_E1000E) wr(e, REG_CTRL_EXT, rd(e, REG_CTRL_EXT) | (1u << 28));  /* DRV_LOAD */
    if (!(e->flags & F_PCH)) {
        /* power the PHY up (some boards leave it down) and restart auto-negotiation */
        int bmcr = phy_read(e, MII_BMCR);
        if (bmcr >= 0 && bmcr != 0xFFFF && (gen == GEN_IGB || (bmcr & (1 << 11))))
            phy_write(e, MII_BMCR, (uint16_t)((bmcr & ~(1 << 11)) | (1 << 12) | (1 << 9)));
    }

    read_mac(e);
    for (int i = 0; i < 128; i++) wr(e, REG_MTA + (uint32_t)i * 4, 0);

    uint64_t rx_phys, tx_phys, rxb_phys, txb_phys;
    e->rx = dma_alloc(NRX * sizeof(struct rx_desc), &rx_phys);
    e->tx = dma_alloc(NTX * sizeof(struct tx_desc), &tx_phys);
    e->rxbuf = dma_alloc(NRX * BUFSZ, &rxb_phys);
    e->txbuf = dma_alloc(NTX * BUFSZ, &txb_phys);
    if (!e->rx || !e->tx || !e->rxbuf || !e->txbuf) return NULL;

    /* receive */
    for (int i = 0; i < NRX; i++) e->rx[i].addr = rxb_phys + (uint64_t)i * BUFSZ;
    qwr(e, Q_RDBAL, (uint32_t)rx_phys);
    qwr(e, Q_RDBAH, (uint32_t)(rx_phys >> 32));
    qwr(e, Q_RDLEN, NRX * sizeof(struct rx_desc));
    qwr(e, Q_RDH, 0);
    qwr(e, Q_RDT, 0);
    if (gen == GEN_IGB) {
        qwr(e, Q_SRRCTL, 2 | (1u << 25) | (1u << 31));  /* 2 KiB buffers, advanced one-buffer descriptors, drop when full */
        qwr(e, Q_RXDCTL, qrd(e, Q_RXDCTL) | (1u << 25));
        if (!wait_bit(e, Q_RXDCTL, 1u << 25)) klog("e1000: receive queue did not enable");
    }
    qwr(e, Q_RDT, NRX - 1);
    wr(e, REG_RCTL, (1u << 1) | (1u << 15) | (1u << 26));      /* EN, BAM, SECRC, 2048-byte buffers */

    /* transmit */
    for (int i = 0; i < NTX; i++) {
        e->tx[i].addr = txb_phys + (uint64_t)i * BUFSZ;
        e->tx[i].cmd = 0;
        e->tx[i].status = 1;
    }
    qwr(e, Q_TDBAL, (uint32_t)tx_phys);
    qwr(e, Q_TDBAH, (uint32_t)(tx_phys >> 32));
    qwr(e, Q_TDLEN, NTX * sizeof(struct tx_desc));
    qwr(e, Q_TDH, 0);
    qwr(e, Q_TDT, 0);
    if (gen == GEN_E1000E) {
        /* descriptor write-back granularity as the 8257x/ICH manuals ask for */
        qwr(e, Q_TXDCTL, (qrd(e, Q_TXDCTL) & ~0x003F0000u) | 0x01010000u | 0x00400000u);
    } else if (gen == GEN_IGB) {
        qwr(e, Q_TXDCTL, qrd(e, Q_TXDCTL) | (1u << 25));
        if (!wait_bit(e, Q_TXDCTL, 1u << 25)) klog("e1000: transmit queue did not enable");
    }
    wr(e, REG_TCTL, (1u << 1) | (1u << 3) | (0x0Fu << 4) | (0x40u << 12));   /* EN, PSP, CT, COLD */
    wr(e, REG_TIPG, 10 | (8 << 10) | (6 << 20));

    snprintf(e->nd.name, sizeof(e->nd.name), "Intel %s", id->model);
    e->nd.send = e1k_send;
    e->nd.poll = e1k_poll;
    e->nd.link = e1k_link;
    e->nd.priv = e;
    return &e->nd;
}

static struct netdev *attach_8254x(struct pci_dev *p, const struct nic_id *id) { return e1k_attach(p, id, GEN_8254X); }
static struct netdev *attach_e1000e(struct pci_dev *p, const struct nic_id *id) { return e1k_attach(p, id, GEN_E1000E); }
static struct netdev *attach_igb(struct pci_dev *p, const struct nic_id *id) { return e1k_attach(p, id, GEN_IGB); }

static const struct nic_id ids_8254x[] = {
    { 0x8086, 0x1004, "82543GC Gigabit Ethernet", F_NO_EERD },
    { 0x8086, 0x1008, "82544EI Gigabit Ethernet", F_NO_EERD },
    { 0x8086, 0x100C, "82544GC Gigabit Ethernet", F_NO_EERD },
    { 0x8086, 0x100D, "82544GC Gigabit Ethernet (LOM)", F_NO_EERD },
    { 0x8086, 0x100E, "82540EM Gigabit Ethernet", 0 },
    { 0x8086, 0x1015, "82540EM Gigabit Ethernet (LOM)", 0 },
    { 0x8086, 0x1016, "82540EP Gigabit Ethernet (LOM)", 0 },
    { 0x8086, 0x1017, "82540EP Gigabit Ethernet", 0 },
    { 0x8086, 0x101E, "82540EP Gigabit Ethernet (mobile)", 0 },
    { 0x8086, 0x100F, "82545EM Gigabit Ethernet", 0 },
    { 0x8086, 0x1011, "82545EM Gigabit Ethernet (fiber)", 0 },
    { 0x8086, 0x1026, "82545GM Gigabit Ethernet", 0 },
    { 0x8086, 0x1010, "82546EB Dual Port Gigabit Ethernet", 0 },
    { 0x8086, 0x1012, "82546EB Dual Port Gigabit Ethernet (fiber)", 0 },
    { 0x8086, 0x1079, "82546GB Dual Port Gigabit Ethernet", 0 },
    { 0x8086, 0x1013, "82541EI Gigabit Ethernet", F_EERD_SHIFT2 },
    { 0x8086, 0x1018, "82541EI Gigabit Ethernet (mobile)", F_EERD_SHIFT2 },
    { 0x8086, 0x1076, "82541GI Gigabit Ethernet", F_EERD_SHIFT2 },
    { 0x8086, 0x1077, "82541GI Gigabit Ethernet (mobile)", F_EERD_SHIFT2 },
    { 0x8086, 0x1078, "82541ER Gigabit Ethernet", F_EERD_SHIFT2 },
    { 0x8086, 0x107C, "82541PI Gigabit Ethernet", F_EERD_SHIFT2 },
    { 0x8086, 0x1019, "82547EI Gigabit Ethernet", F_EERD_SHIFT2 },
    { 0x8086, 0x1075, "82547GI Gigabit Ethernet", F_EERD_SHIFT2 },
    { 0 },
};

static const struct nic_id ids_e1000e[] = {
    { 0x8086, 0x105E, "82571EB Dual Port Gigabit Ethernet", F_EERD_SHIFT2 },
    { 0x8086, 0x105F, "82571EB Dual Port Gigabit Ethernet (fiber)", F_EERD_SHIFT2 },
    { 0x8086, 0x10A4, "82571EB Quad Port Gigabit Ethernet", F_EERD_SHIFT2 },
    { 0x8086, 0x107D, "82572EI Gigabit Ethernet", F_EERD_SHIFT2 },
    { 0x8086, 0x107E, "82572EI Gigabit Ethernet (fiber)", F_EERD_SHIFT2 },
    { 0x8086, 0x10B9, "82572EI Gigabit Ethernet", F_EERD_SHIFT2 },
    { 0x8086, 0x108B, "82573V Gigabit Ethernet", F_EERD_SHIFT2 },
    { 0x8086, 0x108C, "82573E Gigabit Ethernet", F_EERD_SHIFT2 },
    { 0x8086, 0x109A, "82573L Gigabit Ethernet", F_EERD_SHIFT2 },
    { 0x8086, 0x10D3, "82574L Gigabit Ethernet", F_EERD_SHIFT2 },
    { 0x8086, 0x10F6, "82574LA Gigabit Ethernet", F_EERD_SHIFT2 },
    { 0x8086, 0x150C, "82583V Gigabit Ethernet", F_EERD_SHIFT2 },
    { 0x8086, 0x1049, "82566MM Gigabit Ethernet (ICH8)", F_PCH },
    { 0x8086, 0x104A, "82566DM Gigabit Ethernet (ICH8)", F_PCH },
    { 0x8086, 0x104B, "82566DC Gigabit Ethernet (ICH8)", F_PCH },
    { 0x8086, 0x104D, "82566MC Gigabit Ethernet (ICH8)", F_PCH },
    { 0x8086, 0x10BD, "82566DM-2 Gigabit Ethernet (ICH9)", F_PCH },
    { 0x8086, 0x294C, "82566DC-2 Gigabit Ethernet (ICH9)", F_PCH },
    { 0x8086, 0x10E5, "82567LM-4 Gigabit Ethernet (ICH9)", F_PCH },
    { 0x8086, 0x10DE, "82567LM-3 Gigabit Ethernet (ICH10)", F_PCH },
    { 0x8086, 0x10DF, "82567LF-3 Gigabit Ethernet (ICH10)", F_PCH },
    { 0x8086, 0x10CC, "82567LM-2 Gigabit Ethernet (ICH10)", F_PCH },
    { 0x8086, 0x10CD, "82567LF-2 Gigabit Ethernet (ICH10)", F_PCH },
    { 0x8086, 0x10CE, "82567V-2 Gigabit Ethernet (ICH10)", F_PCH },
    { 0x8086, 0x10EA, "82577LM Gigabit Ethernet", F_PCH },
    { 0x8086, 0x10EB, "82577LC Gigabit Ethernet", F_PCH },
    { 0x8086, 0x10EF, "82578DM Gigabit Ethernet", F_PCH },
    { 0x8086, 0x10F0, "82578DC Gigabit Ethernet", F_PCH },
    { 0x8086, 0x1502, "82579LM Gigabit Ethernet", F_PCH },
    { 0x8086, 0x1503, "82579V Gigabit Ethernet", F_PCH },
    { 0x8086, 0x153A, "Ethernet Connection I217-LM", F_PCH },
    { 0x8086, 0x153B, "Ethernet Connection I217-V", F_PCH },
    { 0x8086, 0x155A, "Ethernet Connection I218-LM", F_PCH },
    { 0x8086, 0x1559, "Ethernet Connection I218-V", F_PCH },
    { 0x8086, 0x15A0, "Ethernet Connection (2) I218-LM", F_PCH },
    { 0x8086, 0x15A1, "Ethernet Connection (2) I218-V", F_PCH },
    { 0x8086, 0x15A2, "Ethernet Connection (3) I218-LM", F_PCH },
    { 0x8086, 0x15A3, "Ethernet Connection (3) I218-V", F_PCH },
    { 0x8086, 0x156F, "Ethernet Connection I219-LM", F_PCH },
    { 0x8086, 0x1570, "Ethernet Connection I219-V", F_PCH },
    { 0x8086, 0x15B7, "Ethernet Connection (2) I219-LM", F_PCH },
    { 0x8086, 0x15B8, "Ethernet Connection (2) I219-V", F_PCH },
    { 0x8086, 0x15D7, "Ethernet Connection (4) I219-LM", F_PCH },
    { 0x8086, 0x15D8, "Ethernet Connection (4) I219-V", F_PCH },
    { 0x8086, 0x15E3, "Ethernet Connection (5) I219-LM", F_PCH },
    { 0x8086, 0x15D6, "Ethernet Connection (5) I219-V", F_PCH },
    { 0x8086, 0x15BD, "Ethernet Connection (6) I219-LM", F_PCH },
    { 0x8086, 0x15BE, "Ethernet Connection (6) I219-V", F_PCH },
    { 0x8086, 0x15BB, "Ethernet Connection (7) I219-LM", F_PCH },
    { 0x8086, 0x15BC, "Ethernet Connection (7) I219-V", F_PCH },
    { 0 },
};

static const struct nic_id ids_igb[] = {
    { 0x8086, 0x10A7, "82575EB Gigabit Ethernet", F_EERD_SHIFT2 },
    { 0x8086, 0x10A9, "82575EB Gigabit Ethernet (fiber)", F_EERD_SHIFT2 },
    { 0x8086, 0x10D6, "82575GB Quad Port Gigabit Ethernet", F_EERD_SHIFT2 },
    { 0x8086, 0x10C9, "82576 Gigabit Ethernet", F_EERD_SHIFT2 },
    { 0x8086, 0x10E6, "82576 Gigabit Ethernet (fiber)", F_EERD_SHIFT2 },
    { 0x8086, 0x10E7, "82576 Gigabit Ethernet (SerDes)", F_EERD_SHIFT2 },
    { 0x8086, 0x10E8, "82576 Quad Port Gigabit Ethernet", F_EERD_SHIFT2 },
    { 0x8086, 0x1526, "82576 Quad Port Gigabit Ethernet (ET2)", F_EERD_SHIFT2 },
    { 0x8086, 0x150A, "82576NS Gigabit Ethernet", F_EERD_SHIFT2 },
    { 0x8086, 0x150E, "82580 Gigabit Ethernet", F_EERD_SHIFT2 },
    { 0x8086, 0x150F, "82580 Gigabit Ethernet (fiber)", F_EERD_SHIFT2 },
    { 0x8086, 0x1516, "82580 Dual Port Gigabit Ethernet", F_EERD_SHIFT2 },
    { 0x8086, 0x1521, "I350 Gigabit Ethernet", F_EERD_SHIFT2 },
    { 0x8086, 0x1522, "I350 Gigabit Ethernet (fiber)", F_EERD_SHIFT2 },
    { 0x8086, 0x1523, "I350 Gigabit Ethernet (SerDes)", F_EERD_SHIFT2 },
    { 0x8086, 0x1533, "I210 Gigabit Ethernet", F_EERD_SHIFT2 },
    { 0x8086, 0x1536, "I210 Gigabit Ethernet (fiber)", F_EERD_SHIFT2 },
    { 0x8086, 0x1537, "I210 Gigabit Ethernet (SerDes)", F_EERD_SHIFT2 },
    { 0x8086, 0x157B, "I210 Gigabit Ethernet (flashless)", F_EERD_SHIFT2 },
    { 0x8086, 0x1539, "I211 Gigabit Ethernet", F_EERD_SHIFT2 },
    { 0 },
};

const struct nic_driver drv_e1000 = { "e1000", "Intel PRO/1000 (8254x)", ids_8254x, attach_8254x };
const struct nic_driver drv_e1000e = { "e1000e", "Intel PRO/1000 PCIe, ICH/PCH LAN (8257x, 82583, I217-I219)", ids_e1000e, attach_e1000e };
const struct nic_driver drv_igb = { "igb", "Intel Gigabit (82575/82576/82580, I210/I211/I350)", ids_igb, attach_igb };

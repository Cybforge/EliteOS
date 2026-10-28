/*
 * DEC "Tulip" 21140/21142/21143: descriptor rings addressed through CSR3
 * and CSR4, the address filter loaded by transmitting a setup frame, the
 * MAC address read from the serial ROM.
 */
#include <kernel.h>
#include <dev.h>
#include <mm.h>
#include <spinlock.h>
#include <x86.h>
#include "../netdev.h"

#define CSR0  0x00      /* bus mode */
#define CSR1  0x08      /* transmit poll demand */
#define CSR2  0x10      /* receive poll demand */
#define CSR3  0x18      /* receive list base */
#define CSR4  0x20      /* transmit list base */
#define CSR5  0x28      /* status */
#define CSR6  0x30      /* operation mode */
#define CSR7  0x38      /* interrupt enable */
#define CSR9  0x48      /* serial ROM, MII management */

#define OWN   (1u << 31)
#define RER   (1u << 25)    /* receive end of ring */
#define TER   (1u << 25)    /* transmit end of ring */
#define T_LS  (1u << 30)
#define T_FS  (1u << 29)
#define T_SET (1u << 27)    /* setup frame */
#define R_ES  (1u << 15)
#define R_FS  (1u << 9)
#define R_LS  (1u << 8)

#define NRX 128
#define NTX 32
#define BUFSZ 1536

struct desc { uint32_t status, control, buf1, buf2; };

struct tulip {
    struct netdev nd;
    uint16_t io;
    struct desc *rx, *tx;
    uint8_t *rxbuf, *txbuf;
    unsigned rx_cur, tx_cur;
    int phy;
    spinlock_t lock;
};

static uint32_t csr_r(struct tulip *t, int r) { return inl((uint16_t)(t->io + r)); }
static void csr_w(struct tulip *t, int r, uint32_t v) { outl((uint16_t)(t->io + r), v); }

static bool tulip_send(struct netdev *nd, const void *data, size_t len)
{
    struct tulip *t = nd->priv;
    if (len > BUFSZ) return false;
    spin_lock(&t->lock);
    struct desc *d = &t->tx[t->tx_cur];
    for (int i = 0; i < 100000 && (((volatile struct desc *)d)->status & OWN); i++) cpu_relax();
    if (d->status & OWN) { spin_unlock(&t->lock); return false; }
    memcpy(t->txbuf + (size_t)t->tx_cur * BUFSZ, data, len);
    d->control = T_LS | T_FS | (uint32_t)len | (t->tx_cur == NTX - 1 ? TER : 0);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    d->status = OWN;
    t->tx_cur = (t->tx_cur + 1) % NTX;
    csr_w(t, CSR1, 1);
    spin_unlock(&t->lock);
    return true;
}

static int tulip_poll(struct netdev *nd, void *buf, size_t cap)
{
    struct tulip *t = nd->priv;
    for (;;) {
        struct desc *d = &t->rx[t->rx_cur];
        uint32_t st = ((volatile struct desc *)d)->status;
        if (st & OWN) return 0;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        int len = 0;
        if (!(st & R_ES) && (st & R_FS) && (st & R_LS)) {
            int n = (int)((st >> 16) & 0x3FFF) - 4;           /* frame length includes the CRC */
            if (n > 0) {
                len = MIN(n, (int)cap);
                memcpy(buf, t->rxbuf + (size_t)t->rx_cur * BUFSZ, (size_t)len);
            }
        }
        __atomic_thread_fence(__ATOMIC_RELEASE);
        d->status = OWN;
        t->rx_cur = (t->rx_cur + 1) % NRX;
        csr_w(t, CSR2, 1);
        if (len) return len;
    }
}

/* MII management through CSR9: MDC bit 16, MDO bit 17, read mode bit 18, MDI bit 19 */
static int mdio_read(struct tulip *t, int phy, int reg)
{
    uint32_t cmd = (0xF6u << 10) | ((uint32_t)phy << 5) | (uint32_t)reg;
    for (int i = 0; i < 32; i++) {
        csr_w(t, CSR9, 0x20000);
        udelay(1);
        csr_w(t, CSR9, 0x20000 | 0x10000);
        udelay(1);
    }
    for (int i = 15; i >= 0; i--) {
        uint32_t bit = (cmd >> i) & 1 ? 0x20000 : 0;
        csr_w(t, CSR9, bit);
        udelay(1);
        csr_w(t, CSR9, bit | 0x10000);
        udelay(1);
    }
    uint32_t v = 0;
    for (int i = 0; i < 19; i++) {
        csr_w(t, CSR9, 0x40000);
        udelay(1);
        v = (v << 1) | ((csr_r(t, CSR9) & 0x80000) ? 1 : 0);
        csr_w(t, CSR9, 0x40000 | 0x10000);
        udelay(1);
    }
    return (int)((v >> 1) & 0xFFFF);
}

static bool tulip_link(struct netdev *nd)
{
    struct tulip *t = nd->priv;
    if (t->phy < 0) return true;
    spin_lock(&t->lock);
    int bmsr = mdio_read(t, t->phy, MII_BMSR);
    spin_unlock(&t->lock);
    return bmsr & BMSR_LINK;
}

/* serial ROM lines in CSR9: select ROM (bit 11) and read (bit 14); CS bit 0, SK bit 1, DI bit 2, DO bit 3 */
static void srom_out(void *ctx, bool cs, bool sk, bool di)
{
    struct tulip *t = ctx;
    csr_w(t, CSR9, 0x4800 | (cs ? 1 : 0) | (sk ? 2 : 0) | (di ? 4 : 0));
    csr_r(t, CSR9);
}

static bool srom_in(void *ctx)
{
    struct tulip *t = ctx;
    return csr_r(t, CSR9) & 8;
}

static struct netdev *tulip_attach(struct pci_dev *p, const struct nic_id *id)
{
    if (!pci_bar_is_io(p, 0)) return NULL;
    struct tulip *t = kzalloc(sizeof(*t));
    t->io = (uint16_t)pci_bar_addr(p, 0);
    t->lock = (spinlock_t)SPINLOCK_INIT("tulip");
    pci_enable_busmaster(p);

    csr_w(t, CSR0, 1);                              /* software reset */
    mdelay(1);
    csr_w(t, CSR0, 0x4800);                         /* 8-longword cache alignment, bursts of 8 */
    csr_w(t, CSR7, 0);

    /* the station address sits at byte 20 of the SROM */
    struct microwire rom = { t, srom_out, srom_in, 0 };
    for (int i = 0; i < 3; i++) {
        uint16_t w = microwire_read(&rom, 10 + i);
        t->nd.mac[i * 2] = (uint8_t)w;
        t->nd.mac[i * 2 + 1] = (uint8_t)(w >> 8);
    }

    uint64_t rx_phys, tx_phys;
    t->rx = dma_alloc(NRX * sizeof(struct desc), &rx_phys);
    t->tx = dma_alloc(NTX * sizeof(struct desc), &tx_phys);
    t->rxbuf = dma_alloc(NRX * BUFSZ, NULL);
    t->txbuf = dma_alloc(NTX * BUFSZ, NULL);
    if (!t->rx || !t->tx || !t->rxbuf || !t->txbuf) return NULL;
    for (int i = 0; i < NRX; i++) {
        t->rx[i].buf1 = dma32(t->rxbuf + (size_t)i * BUFSZ);
        t->rx[i].control = BUFSZ | (i == NRX - 1 ? RER : 0);
        t->rx[i].status = OWN;
    }
    for (int i = 0; i < NTX; i++) {
        t->tx[i].buf1 = dma32(t->txbuf + (size_t)i * BUFSZ);
        t->tx[i].control = i == NTX - 1 ? TER : 0;
    }
    csr_w(t, CSR3, (uint32_t)rx_phys);
    csr_w(t, CSR4, (uint32_t)tx_phys);

    t->phy = -1;
    for (int phy = 1; phy < 32 && t->phy < 0; phy++) {
        int v = mdio_read(t, phy, MII_BMSR);
        if (v != 0xFFFF && v != 0) t->phy = phy;
    }
    /* store and forward, MII/SYM port; full duplex only when the PHY negotiated it */
    uint32_t mode = (1u << 21) | (1u << 18);
    if (t->phy >= 0 && (mdio_read(t, t->phy, 4) & mdio_read(t, t->phy, 5) & 0x0140)) mode |= 1u << 9;
    csr_w(t, CSR6, mode | (1u << 13));              /* start the transmitter for the setup frame */

    /* setup frame: perfect filtering of 16 addresses, ours and broadcast */
    uint8_t *sf = t->txbuf;
    memset(sf, 0, 192);
    for (int e = 0; e < 16; e++) {
        const uint8_t *m = e == 1 ? (const uint8_t *)"\xFF\xFF\xFF\xFF\xFF\xFF" : t->nd.mac;
        for (int w = 0; w < 3; w++) {
            sf[e * 12 + w * 4] = m[w * 2];
            sf[e * 12 + w * 4 + 1] = m[w * 2 + 1];
        }
    }
    struct desc *d = &t->tx[0];
    d->control = T_SET | 192;
    d->status = OWN;
    csr_w(t, CSR1, 1);
    for (int i = 0; i < 1000 && (((volatile struct desc *)d)->status & OWN); i++) udelay(10);
    if (d->status & OWN) klog("tulip: setup frame was not taken");
    d->control = 0;
    t->tx_cur = 1;

    csr_w(t, CSR6, mode | (1u << 13) | (1u << 1));  /* start receiving */
    csr_w(t, CSR5, 0xFFFFFFFF);

    snprintf(t->nd.name, sizeof(t->nd.name), "%s", id->model);
    t->nd.send = tulip_send;
    t->nd.poll = tulip_poll;
    t->nd.link = tulip_link;
    t->nd.priv = t;
    return &t->nd;
}

static const struct nic_id ids[] = {
    { 0x1011, 0x0009, "DEC 21140/21140A Fast Ethernet (Tulip)" },
    { 0x1011, 0x0019, "DEC 21142/21143 Fast Ethernet (Tulip)" },
    { 0 },
};

const struct nic_driver drv_tulip = { "tulip", "DEC Tulip (21140, 21140A, 21142, 21143)", ids, tulip_attach };

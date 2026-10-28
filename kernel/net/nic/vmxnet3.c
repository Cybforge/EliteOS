/*
 * VMware vmxnet3 (VMware Workstation/ESXi, QEMU): the driver describes its
 * rings in a "shared" structure, activates the device with a command,
 * then hands buffers over with generation bits and producer registers.
 * One transmit and one receive queue, interrupts disabled.
 */
#include <kernel.h>
#include <dev.h>
#include <mm.h>
#include <spinlock.h>
#include <x86.h>
#include "../netdev.h"

/* BAR0 */
#define PT_TXPROD  0x600
#define PT_RXPROD  0x800
#define PT_RXPROD2 0xA00
/* BAR1 */
#define VD_VRRS 0x00
#define VD_UVRS 0x08
#define VD_DSAL 0x10
#define VD_DSAH 0x18
#define VD_CMD  0x20
#define VD_MACL 0x28
#define VD_MACH 0x30

#define CMD_ACTIVATE   0xCAFE0000u
#define CMD_RESET      0xCAFE0002u
#define CMD_UPDATE_RXMODE 0xCAFE0003u
#define CMD_GET_LINK   0xF00D0002u

#define NTX 64
#define NRX 128
#define BUFSZ 2048

struct PACKED shared {
    uint32_t magic, pad;
    /* misc */
    uint32_t version, gos, rev_spt, upt_spt;
    uint64_t upt_features, dd_pa, queue_desc_pa;
    uint32_t dd_len, queue_desc_len, mtu;
    uint16_t max_rx_sg;
    uint8_t num_tx, num_rx;
    uint32_t misc_reserved[4];
    /* interrupts */
    uint8_t auto_mask, num_intrs, event_intr, mod_levels[25];
    uint32_t intr_ctrl, intr_reserved[2];
    /* receive filter */
    uint32_t rx_mode;
    uint16_t mf_table_len, pad1;
    uint64_t mf_table_pa;
    uint32_t vf_table[128];
    /* rss, pm, plugin: version, length, address */
    uint32_t var_conf[3][4];
    uint32_t ecr, reserved[5];
};

struct PACKED txq_desc {
    uint32_t num_deferred, threshold;
    uint64_t ctrl_reserved;
    uint64_t tx_ring, data_ring, comp_ring, dd_pa, conf_reserved;
    uint32_t tx_size, data_size, comp_size, dd_len;
    uint8_t intr, pad1;
    uint16_t data_desc_size;
    uint8_t pad2[4];
    uint8_t stopped, pad3[3];
    uint32_t error;
    uint8_t stats[80];
    uint8_t pad[88];
};

struct PACKED rxq_desc {
    uint8_t update_prod, pad0[7];
    uint64_t ctrl_reserved;
    uint64_t rx_ring[2], comp_ring, dd_pa, data_ring;
    uint32_t rx_size[2], comp_size, dd_len;
    uint8_t intr, pad1;
    uint16_t data_desc_size;
    uint8_t pad2[4];
    uint8_t stopped, pad3[3];
    uint32_t error;
    uint8_t stats[80];
    uint8_t pad[88];
};

struct txd { uint64_t addr; uint32_t w2, w3; };
struct txcd { uint32_t w0, w1, w2, w3; };
struct rxd { uint64_t addr; uint32_t w2, w3; };
struct rxcd { uint32_t w0, w1, w2, w3; };

struct vmx {
    struct netdev nd;
    volatile uint8_t *bar0, *bar1;
    struct shared *sh;
    struct txq_desc *tq;
    struct rxq_desc *rq;
    struct txd *tx;
    struct txcd *txc;
    struct rxd *rx, *rx2;
    struct rxcd *rxc;
    uint8_t *txbuf, *rxbuf;
    unsigned tx_next, tx_gen, txc_next, txc_gen, tx_inflight;
    unsigned rx_fill, rx_fill_gen, rxc_next, rxc_gen;
    spinlock_t lock;
};

static uint32_t cmd(struct vmx *v, uint32_t c)
{
    mmio_w32(v->bar1, VD_CMD, c);
    return mmio_r32(v->bar1, VD_CMD);
}

static void tx_reclaim(struct vmx *v)
{
    for (;;) {
        struct txcd *c = &v->txc[v->txc_next];
        if ((((volatile struct txcd *)c)->w3 >> 31) != v->txc_gen) break;
        v->tx_inflight--;
        if (++v->txc_next == NTX) { v->txc_next = 0; v->txc_gen ^= 1; }
    }
}

static bool vmx_send(struct netdev *nd, const void *data, size_t len)
{
    struct vmx *v = nd->priv;
    if (len > BUFSZ) return false;
    spin_lock(&v->lock);
    for (int i = 0; i < 200000; i++) {
        tx_reclaim(v);
        if (v->tx_inflight < NTX - 1) break;
        cpu_relax();
    }
    if (v->tx_inflight >= NTX - 1) { spin_unlock(&v->lock); return false; }
    unsigned i = v->tx_next;
    memcpy(v->txbuf + (size_t)i * BUFSZ, data, len);
    struct txd *d = &v->tx[i];
    d->addr = V2P(v->txbuf + (size_t)i * BUFSZ);
    d->w3 = (1u << 12) | (1u << 13);                   /* EOP, completion wanted */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    d->w2 = (uint32_t)len | (v->tx_gen << 14);          /* the generation bit hands it over */
    v->tx_inflight++;
    if (++v->tx_next == NTX) { v->tx_next = 0; v->tx_gen ^= 1; }
    v->tq->num_deferred++;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    mmio_w32(v->bar0, PT_TXPROD, v->tx_next);
    spin_unlock(&v->lock);
    return true;
}

static void rx_refill(struct vmx *v)
{
    struct rxd *d = &v->rx[v->rx_fill];
    d->addr = V2P(v->rxbuf + (size_t)v->rx_fill * BUFSZ);
    d->w3 = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    d->w2 = BUFSZ | (v->rx_fill_gen << 31);             /* head buffer, generation */
    if (++v->rx_fill == NRX) { v->rx_fill = 0; v->rx_fill_gen ^= 1; }
}

static int vmx_poll(struct netdev *nd, void *buf, size_t cap)
{
    struct vmx *v = nd->priv;
    int got = 0;
    spin_lock(&v->lock);
    while (!got) {
        struct rxcd *c = &v->rxc[v->rxc_next];
        uint32_t w3 = ((volatile struct rxcd *)c)->w3;
        if ((w3 >> 31) != v->rxc_gen) break;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        uint32_t idx = c->w0 & 0xFFF, qid = (c->w0 >> 16) & 0x3FF;
        uint32_t len = c->w2 & 0x3FFF;
        bool err = c->w2 & (1u << 14), eop = c->w0 & (1u << 14), sop = c->w0 & (1u << 15);
        if (++v->rxc_next == 2 * NRX) { v->rxc_next = 0; v->rxc_gen ^= 1; }
        if (qid != 0 || idx >= NRX) continue;           /* ring 2 is never filled */
        if (!err && sop && eop && len) {
            got = (int)MIN(len, (uint32_t)cap);
            memcpy(buf, v->rxbuf + (size_t)idx * BUFSZ, (size_t)got);
        }
        rx_refill(v);
    }
    if (v->rq->update_prod) mmio_w32(v->bar0, PT_RXPROD, v->rx_fill);
    spin_unlock(&v->lock);
    return got;
}

static bool vmx_link(struct netdev *nd)
{
    struct vmx *v = nd->priv;
    spin_lock(&v->lock);
    bool up = cmd(v, CMD_GET_LINK) & 1;
    spin_unlock(&v->lock);
    return up;
}

static struct netdev *vmx_attach(struct pci_dev *p, const struct nic_id *id)
{
    uint64_t b0 = pci_bar_addr(p, 0), b1 = pci_bar_addr(p, 1);
    if (!b0 || !b1 || pci_bar_is_io(p, 0) || pci_bar_is_io(p, 1)) return NULL;
    struct vmx *v = kzalloc(sizeof(*v));
    v->lock = (spinlock_t)SPINLOCK_INIT("vmxnet3");
    v->bar0 = vmm_map_mmio(b0, 4096);
    v->bar1 = vmm_map_mmio(b1, 4096);
    pci_enable_busmaster(p);

    if (!(mmio_r32(v->bar1, VD_VRRS) & 1)) return NULL;
    mmio_w32(v->bar1, VD_VRRS, 1);
    if (!(mmio_r32(v->bar1, VD_UVRS) & 1)) return NULL;
    mmio_w32(v->bar1, VD_UVRS, 1);
    cmd(v, CMD_RESET);
    uint32_t macl = mmio_r32(v->bar1, VD_MACL), mach = mmio_r32(v->bar1, VD_MACH);
    for (int i = 0; i < 4; i++) v->nd.mac[i] = (uint8_t)(macl >> (i * 8));
    v->nd.mac[4] = (uint8_t)mach;
    v->nd.mac[5] = (uint8_t)(mach >> 8);

    uint64_t sh_pa, q_pa;
    v->sh = dma_alloc(sizeof(struct shared), &sh_pa);
    uint8_t *qd = dma_alloc(512, &q_pa);
    v->tx = dma_alloc(NTX * sizeof(struct txd), NULL);
    v->txc = dma_alloc(NTX * sizeof(struct txcd), NULL);
    v->rx = dma_alloc(NRX * sizeof(struct rxd), NULL);
    v->rx2 = dma_alloc(NRX * sizeof(struct rxd), NULL);
    v->rxc = dma_alloc(2 * NRX * sizeof(struct rxcd), NULL);
    v->txbuf = dma_alloc(NTX * BUFSZ, NULL);
    v->rxbuf = dma_alloc(NRX * BUFSZ, NULL);
    if (!v->sh || !qd || !v->tx || !v->txc || !v->rx || !v->rx2 || !v->rxc || !v->txbuf || !v->rxbuf) return NULL;
    v->tq = (struct txq_desc *)qd;
    v->rq = (struct rxq_desc *)(qd + 256);

    struct shared *s = v->sh;
    s->magic = 0xBABEFEE1;
    s->version = 1;
    s->gos = 2 | (1u << 2);                 /* 64-bit, Linux-like guest */
    s->rev_spt = 1;
    s->upt_spt = 1;
    s->queue_desc_pa = q_pa;
    s->queue_desc_len = 512;
    s->mtu = 1500;
    s->max_rx_sg = 1;
    s->num_tx = 1;
    s->num_rx = 1;
    s->auto_mask = 1;
    s->num_intrs = 1;
    s->event_intr = 0;
    s->intr_ctrl = 1;                       /* all interrupts disabled */
    s->rx_mode = 1 | 4 | 8;                 /* unicast, broadcast, all multicast */

    v->tq->threshold = 1;
    v->tq->tx_ring = V2P(v->tx);
    v->tq->comp_ring = V2P(v->txc);
    v->tq->tx_size = NTX;
    v->tq->comp_size = NTX;
    v->rq->rx_ring[0] = V2P(v->rx);
    v->rq->rx_ring[1] = V2P(v->rx2);
    v->rq->comp_ring = V2P(v->rxc);
    v->rq->rx_size[0] = NRX;
    v->rq->rx_size[1] = NRX;
    v->rq->comp_size = 2 * NRX;

    v->tx_gen = v->txc_gen = v->rxc_gen = v->rx_fill_gen = 1;
    for (int i = 0; i < NRX; i++) rx_refill(v);

    mmio_w32(v->bar1, VD_DSAL, (uint32_t)sh_pa);
    mmio_w32(v->bar1, VD_DSAH, (uint32_t)(sh_pa >> 32));
    uint32_t r = cmd(v, CMD_ACTIVATE);
    if (r) {
        klog("vmxnet3: activation failed (%u)", r);
        return NULL;
    }
    mmio_w32(v->bar0, PT_RXPROD, 0);
    mmio_w32(v->bar0, PT_RXPROD2, 0);
    cmd(v, CMD_UPDATE_RXMODE);

    snprintf(v->nd.name, sizeof(v->nd.name), "%s", id->model);
    v->nd.send = vmx_send;
    v->nd.poll = vmx_poll;
    v->nd.link = vmx_link;
    v->nd.priv = v;
    return &v->nd;
}

static const struct nic_id ids[] = {
    { 0x15AD, 0x07B0, "VMware vmxnet3 paravirtual Ethernet" },
    { 0 },
};

const struct nic_driver drv_vmxnet3 = { "vmxnet3", "VMware vmxnet3 (VMware Workstation, ESXi)", ids, vmx_attach };

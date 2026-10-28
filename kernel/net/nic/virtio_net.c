/*
 * virtio-net (QEMU/KVM, cloud VMs, VirtualBox): both the virtio 1.0 PCI
 * interface (capabilities pointing into BARs) and the legacy I/O-port
 * interface of older hypervisors. Split virtqueues, queue 0 receives,
 * queue 1 transmits, one buffer per frame with the net header in front.
 */
#include <kernel.h>
#include <dev.h>
#include <mm.h>
#include <spinlock.h>
#include <x86.h>
#include "../netdev.h"

#define QSIZE_MAX 256
#define BUFSZ 2048

#define F_MAC       (1ull << 5)
#define F_STATUS    (1ull << 16)
#define F_VERSION_1 (1ull << 32)

#define S_ACK       1
#define S_DRIVER    2
#define S_DRIVER_OK 4
#define S_FEATURES_OK 8

#define VRING_WRITE 2

/* legacy I/O registers */
#define L_HOST_FEATURES  0x00
#define L_GUEST_FEATURES 0x04
#define L_QUEUE_PFN      0x08
#define L_QUEUE_SIZE     0x0C
#define L_QUEUE_SELECT   0x0E
#define L_QUEUE_NOTIFY   0x10
#define L_STATUS         0x12
#define L_ISR            0x13
#define L_CONFIG         0x14

/* virtio 1.0 common configuration */
#define C_DFSELECT   0x00
#define C_DF         0x04
#define C_GFSELECT   0x08
#define C_GF         0x0C
#define C_NUMQ       0x12
#define C_STATUS     0x14
#define C_QSELECT    0x16
#define C_QSIZE      0x18
#define C_QENABLE    0x1C
#define C_QNOTIFYOFF 0x1E
#define C_QDESC      0x20
#define C_QDRIVER    0x28
#define C_QDEVICE    0x30

struct vdesc { uint64_t addr; uint32_t len; uint16_t flags, next; } PACKED;
struct vavail { uint16_t flags, idx; uint16_t ring[]; } PACKED;
struct vused_elem { uint32_t id, len; } PACKED;
struct vused { uint16_t flags, idx; struct vused_elem ring[]; } PACKED;

struct vq {
    unsigned size;
    struct vdesc *desc;
    struct vavail *avail;
    volatile struct vused *used;
    uint16_t last_used;
    uint8_t *bufs;
    volatile uint8_t *notify;       /* modern */
    int index;
};

struct vnet {
    struct netdev nd;
    bool modern;
    uint16_t io;                    /* legacy */
    volatile uint8_t *common, *devcfg;
    struct vq rx, tx;
    size_t hdr_len;                 /* 12 with VERSION_1, 10 for legacy */
    bool has_status;
    unsigned tx_next, tx_inflight;
    spinlock_t lock;
};

static void notify(struct vnet *v, struct vq *q)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (v->modern) *(volatile uint16_t *)q->notify = (uint16_t)q->index;
    else outw((uint16_t)(v->io + L_QUEUE_NOTIFY), (uint16_t)q->index);
}

static void rx_post(struct vnet *v, uint16_t id)
{
    struct vq *q = &v->rx;
    q->avail->ring[q->avail->idx % q->size] = id;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    q->avail->idx++;
}

static bool vnet_send(struct netdev *nd, const void *data, size_t len)
{
    struct vnet *v = nd->priv;
    if (len + v->hdr_len > BUFSZ) return false;
    spin_lock(&v->lock);
    struct vq *q = &v->tx;
    /* reclaim what the device has finished */
    for (int spin = 0; spin < 200000; spin++) {
        while (q->last_used != q->used->idx) { q->last_used++; v->tx_inflight--; }
        if (v->tx_inflight < q->size) break;
        cpu_relax();
    }
    if (v->tx_inflight >= q->size) { spin_unlock(&v->lock); return false; }
    uint16_t id = (uint16_t)(v->tx_next++ % q->size);
    uint8_t *b = q->bufs + (size_t)id * BUFSZ;
    memset(b, 0, v->hdr_len);
    memcpy(b + v->hdr_len, data, len);
    q->desc[id].len = (uint32_t)(v->hdr_len + len);
    q->desc[id].flags = 0;
    q->avail->ring[q->avail->idx % q->size] = id;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    q->avail->idx++;
    v->tx_inflight++;
    notify(v, q);
    spin_unlock(&v->lock);
    return true;
}

static int vnet_poll(struct netdev *nd, void *buf, size_t cap)
{
    struct vnet *v = nd->priv;
    struct vq *q = &v->rx;
    int got = 0;
    spin_lock(&v->lock);
    while (!got && q->last_used != q->used->idx) {
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        struct vused_elem e = q->used->ring[q->last_used % q->size];
        q->last_used++;
        if (e.id < q->size && e.len > v->hdr_len) {
            got = (int)MIN(e.len - v->hdr_len, cap);
            memcpy(buf, q->bufs + (size_t)e.id * BUFSZ + v->hdr_len, (size_t)got);
        }
        if (e.id < q->size) rx_post(v, (uint16_t)e.id);
        notify(v, q);
    }
    spin_unlock(&v->lock);
    return got;
}

static bool vnet_link(struct netdev *nd)
{
    struct vnet *v = nd->priv;
    if (!v->has_status) return true;
    uint16_t st = v->modern ? *(volatile uint16_t *)(v->devcfg + 6) : inw((uint16_t)(v->io + L_CONFIG + 6));
    return st & 1;
}

static bool vq_alloc(struct vq *q, unsigned size, bool legacy_layout, uint64_t *desc_phys, uint64_t *avail_phys,
                     uint64_t *used_phys)
{
    q->size = size;
    size_t desc_sz = size * sizeof(struct vdesc);
    size_t avail_sz = 6 + 2 * size;
    size_t used_off = legacy_layout ? ALIGN_UP(desc_sz + avail_sz, 4096) : ALIGN_UP(desc_sz + avail_sz, 4);
    size_t total = used_off + 6 + 8 * size;
    uint64_t phys;
    uint8_t *mem = dma_alloc(total, &phys);
    q->bufs = dma_alloc((size_t)size * BUFSZ, NULL);
    if (!mem || !q->bufs) return false;
    q->desc = (struct vdesc *)mem;
    q->avail = (struct vavail *)(mem + desc_sz);
    q->used = (volatile struct vused *)(mem + used_off);
    *desc_phys = phys;
    *avail_phys = phys + desc_sz;
    *used_phys = phys + used_off;
    for (unsigned i = 0; i < size; i++) q->desc[i].addr = V2P(q->bufs + (size_t)i * BUFSZ);
    return true;
}

static void fill_rx(struct vnet *v)
{
    for (unsigned i = 0; i < v->rx.size; i++) {
        v->rx.desc[i].len = BUFSZ;
        v->rx.desc[i].flags = VRING_WRITE;
        rx_post(v, (uint16_t)i);
    }
}

/* ---------------------------------------------------------------- legacy */

static bool legacy_setup(struct vnet *v, struct pci_dev *p)
{
    v->io = (uint16_t)pci_bar_addr(p, 0);
    outb((uint16_t)(v->io + L_STATUS), 0);
    outb((uint16_t)(v->io + L_STATUS), S_ACK | S_DRIVER);
    uint32_t host = inl((uint16_t)(v->io + L_HOST_FEATURES));
    uint32_t want = (uint32_t)(host & (F_MAC | F_STATUS));
    outl((uint16_t)(v->io + L_GUEST_FEATURES), want);
    v->has_status = want & F_STATUS;
    v->hdr_len = 10;
    for (int i = 0; i < 2; i++) {
        struct vq *q = i == 0 ? &v->rx : &v->tx;
        outw((uint16_t)(v->io + L_QUEUE_SELECT), (uint16_t)i);
        unsigned size = inw((uint16_t)(v->io + L_QUEUE_SIZE));
        if (!size) return false;
        uint64_t d, a, u;
        if (!vq_alloc(q, size, true, &d, &a, &u)) return false;
        q->index = i;
        outl((uint16_t)(v->io + L_QUEUE_PFN), (uint32_t)(d >> 12));
    }
    for (int i = 0; i < 6; i++)
        v->nd.mac[i] = (want & F_MAC) ? inb((uint16_t)(v->io + L_CONFIG + i)) : (uint8_t)(i == 0 ? 0x02 : rdtsc() >> (i * 5));
    fill_rx(v);
    outb((uint16_t)(v->io + L_STATUS), S_ACK | S_DRIVER | S_DRIVER_OK);
    notify(v, &v->rx);
    return true;
}

/* ---------------------------------------------------------------- virtio 1.0 */

static volatile uint8_t *cap_ptr(struct pci_dev *p, uint8_t cap, uint32_t *extra)
{
    uint32_t w1 = pci_read32(p->bus, p->dev, p->fn, (uint8_t)(cap + 4));
    uint32_t off = pci_read32(p->bus, p->dev, p->fn, (uint8_t)(cap + 8));
    uint32_t len = pci_read32(p->bus, p->dev, p->fn, (uint8_t)(cap + 12));
    if (extra) *extra = pci_read32(p->bus, p->dev, p->fn, (uint8_t)(cap + 16));
    int bar = (int)(w1 & 0xFF);
    if (bar > 5 || pci_bar_is_io(p, bar)) return NULL;
    uint64_t base = pci_bar_addr(p, bar);
    if (!base) return NULL;
    return (volatile uint8_t *)vmm_map_mmio(base + off, MAX(len, 4096u));
}

static bool modern_setup(struct vnet *v, struct pci_dev *p)
{
    volatile uint8_t *notify_base = NULL;
    uint32_t notify_mult = 0;
    for (uint8_t cap = pci_find_cap(p, 0x09, 0); cap; cap = pci_find_cap(p, 0x09, cap)) {
        uint8_t type = (uint8_t)(pci_read32(p->bus, p->dev, p->fn, cap) >> 24);
        if (type == 1 && !v->common) v->common = cap_ptr(p, cap, NULL);
        else if (type == 2 && !notify_base) notify_base = cap_ptr(p, cap, &notify_mult);
        else if (type == 4 && !v->devcfg) v->devcfg = cap_ptr(p, cap, NULL);
    }
    if (!v->common || !notify_base || !v->devcfg) return false;
    volatile uint8_t *c = v->common;
    mmio_w8(c, C_STATUS, 0);
    for (int i = 0; i < 1000 && mmio_r8(c, C_STATUS); i++) udelay(10);
    mmio_w8(c, C_STATUS, S_ACK | S_DRIVER);
    mmio_w32(c, C_DFSELECT, 0);
    uint64_t f = mmio_r32(c, C_DF);
    mmio_w32(c, C_DFSELECT, 1);
    f |= (uint64_t)mmio_r32(c, C_DF) << 32;
    if (!(f & F_VERSION_1)) return false;
    uint64_t want = f & (F_MAC | F_STATUS | F_VERSION_1);
    mmio_w32(c, C_GFSELECT, 0);
    mmio_w32(c, C_GF, (uint32_t)want);
    mmio_w32(c, C_GFSELECT, 1);
    mmio_w32(c, C_GF, (uint32_t)(want >> 32));
    mmio_w8(c, C_STATUS, S_ACK | S_DRIVER | S_FEATURES_OK);
    if (!(mmio_r8(c, C_STATUS) & S_FEATURES_OK)) return false;
    v->has_status = want & F_STATUS;
    v->hdr_len = 12;
    for (int i = 0; i < 2; i++) {
        struct vq *q = i == 0 ? &v->rx : &v->tx;
        mmio_w16(c, C_QSELECT, (uint16_t)i);
        unsigned size = MIN(mmio_r16(c, C_QSIZE), (unsigned)QSIZE_MAX);
        if (!size) return false;
        mmio_w16(c, C_QSIZE, (uint16_t)size);
        uint64_t d, a, u;
        if (!vq_alloc(q, size, false, &d, &a, &u)) return false;
        q->index = i;
        q->notify = notify_base + (uint32_t)mmio_r16(c, C_QNOTIFYOFF) * notify_mult;
        mmio_w32(c, C_QDESC, (uint32_t)d);
        mmio_w32(c, C_QDESC + 4, (uint32_t)(d >> 32));
        mmio_w32(c, C_QDRIVER, (uint32_t)a);
        mmio_w32(c, C_QDRIVER + 4, (uint32_t)(a >> 32));
        mmio_w32(c, C_QDEVICE, (uint32_t)u);
        mmio_w32(c, C_QDEVICE + 4, (uint32_t)(u >> 32));
        mmio_w16(c, C_QENABLE, 1);
    }
    for (int i = 0; i < 6; i++)
        v->nd.mac[i] = (want & F_MAC) ? mmio_r8(v->devcfg, (uint32_t)i) : (uint8_t)(i == 0 ? 0x02 : rdtsc() >> (i * 5));
    fill_rx(v);
    mmio_w8(c, C_STATUS, S_ACK | S_DRIVER | S_FEATURES_OK | S_DRIVER_OK);
    notify(v, &v->rx);
    return true;
}

static struct netdev *vnet_attach(struct pci_dev *p, const struct nic_id *id)
{
    struct vnet *v = kzalloc(sizeof(*v));
    v->lock = (spinlock_t)SPINLOCK_INIT("virtio-net");
    pci_enable_busmaster(p);
    /* prefer the modern interface; transitional devices offer both */
    v->modern = pci_find_cap(p, 0x09, 0) != 0;
    bool ok = v->modern && modern_setup(v, p);
    if (!ok && pci_bar_is_io(p, 0) && id->device < 0x1040) {
        memset(&v->rx, 0, sizeof(v->rx));
        memset(&v->tx, 0, sizeof(v->tx));
        v->modern = false;
        ok = legacy_setup(v, p);
    }
    if (!ok) return NULL;
    snprintf(v->nd.name, sizeof(v->nd.name), "%s%s", id->model, v->modern ? " (virtio 1.0)" : " (legacy virtio)");
    v->nd.send = vnet_send;
    v->nd.poll = vnet_poll;
    v->nd.link = vnet_link;
    v->nd.priv = v;
    return &v->nd;
}

static const struct nic_id ids[] = {
    { 0x1AF4, 0x1000, "virtio-net network device" },
    { 0x1AF4, 0x1041, "virtio-net network device" },
    { 0 },
};

const struct nic_driver drv_virtio_net = { "virtio-net", "virtio-net (QEMU/KVM, cloud VMs, VirtualBox)", ids, vnet_attach };

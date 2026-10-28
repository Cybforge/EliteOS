/*
 * Shared pieces of the network drivers: the driver table and PCI probing,
 * DMA memory, 93Cxx serial EEPROM access.
 */
#include <kernel.h>
#include <mm.h>
#include "../netdev.h"

static const struct nic_driver *const drivers[] = {
    &drv_e1000, &drv_e1000e, &drv_igb, &drv_eepro100, &drv_rtl8139, &drv_r8169,
    &drv_pcnet, &drv_ne2k, &drv_tulip, &drv_virtio_net, &drv_vmxnet3,
};

void *dma_alloc(size_t bytes, uint64_t *phys)
{
    size_t pages = ALIGN_UP(bytes, PAGE_SIZE) / PAGE_SIZE;
    uint64_t pa = pmm_alloc_contig_below(pages, 0x100000000ull);
    if (!pa) return NULL;
    if (phys) *phys = pa;
    return P2V(pa);
}

uint32_t dma32(const void *va)
{
    return (uint32_t)V2P(va);
}

void netdev_log(struct netdev *d, const char *bus)
{
    klog("net: %s (%s, %s), MAC %02x:%02x:%02x:%02x:%02x:%02x, link %s", d->name, d->driver, bus, d->mac[0],
         d->mac[1], d->mac[2], d->mac[3], d->mac[4], d->mac[5], d->link(d) ? "up" : "down");
}

/* attach every PCI network adapter we have a driver for */
void net_probe_pci(void)
{
    for (int i = 0; i < pci_count(); i++) {
        struct pci_dev *p = pci_get(i);
        bool claimed = false;
        for (size_t k = 0; k < ARRAY_SIZE(drivers) && !claimed; k++) {
            for (const struct nic_id *id = drivers[k]->ids; id->vendor; id++) {
                if (id->vendor != p->vendor || id->device != p->device) continue;
                struct netdev *d = drivers[k]->attach(p, id);
                if (d) {
                    d->driver = drivers[k]->name;
                    netdev_register(d);
                    char bus[16];
                    snprintf(bus, sizeof(bus), "pci %02x:%02x.%x", p->bus, p->dev, p->fn);
                    netdev_log(d, bus);
                } else {
                    klog("net: %s: %s at %02x:%02x.%x did not start", drivers[k]->name, id->model, p->bus, p->dev,
                         p->fn);
                }
                claimed = true;
                break;
            }
        }
        if (!claimed && p->cls == 0x02)
            klog("net: no driver for network controller %04x:%04x at %02x:%02x.%x", p->vendor, p->device, p->bus,
                 p->dev, p->fn);
    }
}

/* for `netcards`: every supported model, grouped by driver */
int net_card_list(void (*cb)(void *ctx, const char *driver, const char *family, uint16_t vendor, uint16_t device,
                             const char *model),
                  void *ctx)
{
    int n = 0;
    for (size_t k = 0; k < ARRAY_SIZE(drivers); k++)
        for (const struct nic_id *id = drivers[k]->ids; id->vendor; id++, n++)
            if (cb) cb(ctx, drivers[k]->name, drivers[k]->family, id->vendor, id->device, id->model);
    return n;
}

/* ------------------------------------------------------------------------
 * 93C46/93C56/93C66 EEPROM: start bit, opcode 10 (read), address, then 16
 * data bits. The chip answers a dummy 0 after the last address bit, which
 * tells us how wide the address is.
 * ---------------------------------------------------------------------- */

static void mw_clock(struct microwire *m, bool di)
{
    m->out(m->ctx, true, false, di);
    udelay(2);
    m->out(m->ctx, true, true, di);
    udelay(2);
}

void microwire_probe(struct microwire *m)
{
    m->out(m->ctx, false, false, false);
    udelay(2);
    m->out(m->ctx, true, false, false);
    udelay(2);
    mw_clock(m, 1);
    mw_clock(m, 1);
    mw_clock(m, 0);
    int bits = 0;
    for (; bits < 12; bits++) {
        mw_clock(m, 0);
        if (!m->in(m->ctx)) { bits++; break; }
    }
    for (int i = 0; i < 16; i++) mw_clock(m, 0);
    m->out(m->ctx, false, false, false);
    udelay(2);
    m->abits = bits >= 6 && bits <= 8 ? bits : 6;
}

uint16_t microwire_read(struct microwire *m, int addr)
{
    if (!m->abits) microwire_probe(m);
    m->out(m->ctx, false, false, false);
    udelay(2);
    m->out(m->ctx, true, false, false);
    udelay(2);
    uint32_t cmd = (6u << m->abits) | (uint32_t)addr;
    for (int i = m->abits + 2; i >= 0; i--) mw_clock(m, (cmd >> i) & 1);
    uint16_t v = 0;
    for (int i = 0; i < 16; i++) {
        mw_clock(m, 0);
        v = (uint16_t)(v << 1 | (m->in(m->ctx) ? 1 : 0));
    }
    m->out(m->ctx, false, false, false);
    udelay(2);
    return v;
}

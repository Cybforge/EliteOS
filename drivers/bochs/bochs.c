/*
 * Graphics driver for the Bochs/QEMU "standard VGA" display adapter (and
 * compatible: bochs-display, the VirtualBox VBE adapter). Resolution and
 * colour depth are set through the VBE "dispi" registers; the framebuffer
 * is the adapter's video memory behind BAR 0. The adapter has no real
 * signal timing, so the timing a monitor would need is accepted but not
 * used.
 *
 * This is the reference for how a graphics driver plugs into the system:
 * find the adapter, map its memory, describe it to the display layer with a
 * `struct gpu`, and answer set_mode().
 */
#include <kernel.h>
#include <mm.h>
#include <dev.h>
#include <x86.h>
#include <vfs.h>
#include <module.h>
#include <gpu.h>

#define DISPI_INDEX 0x01CE
#define DISPI_DATA  0x01CF
#define DISPI_ID 0
#define DISPI_XRES 1
#define DISPI_YRES 2
#define DISPI_BPP 3
#define DISPI_ENABLE 4
#define DISPI_BANK 5
#define DISPI_VIRT_WIDTH 6
#define DISPI_VIRT_HEIGHT 7
#define DISPI_X_OFFSET 8
#define DISPI_Y_OFFSET 9
#define DISPI_VIDEO_MEMORY_64K 10
#define DISPI_ID_MIN 0xB0C0
#define DISPI_ENABLED 0x01
#define DISPI_LFB 0x40
#define DISPI_NOCLEARMEM 0x80

struct bochs {
    struct gpu gpu;
    struct pci_dev *pci;
    uint64_t fb_phys, vram;
    void *fb;
    size_t mapped;
    void *mmio;                         /* BAR 2: EDID and the register file, if the adapter has it */
};

static struct bochs *dev;

static void wr(uint16_t reg, uint16_t v) { outw(DISPI_INDEX, reg); outw(DISPI_DATA, v); }
static uint16_t rd(uint16_t reg) { outw(DISPI_INDEX, reg); return inw(DISPI_DATA); }

static int bochs_set_mode(struct gpu *g, const struct gpu_timing *t, struct gpu_fb *out)
{
    struct bochs *b = g->priv;
    uint64_t need = (uint64_t)t->hactive * t->vactive * 4;
    if (need > b->vram) return E_INVAL;
    if (b->mapped < need) {
        /* map what this mode needs plus room for the next larger one */
        size_t want = b->vram < (64u << 20) ? b->vram : (64u << 20);
        b->fb = vmm_map_mmio_wc(b->fb_phys, want);
        if (!b->fb) return E_NOMEM;
        b->mapped = want;
    }
    wr(DISPI_ENABLE, 0);
    wr(DISPI_BPP, 32);
    wr(DISPI_XRES, t->hactive);
    wr(DISPI_YRES, t->vactive);
    wr(DISPI_VIRT_WIDTH, t->hactive);
    wr(DISPI_VIRT_HEIGHT, t->vactive);
    wr(DISPI_X_OFFSET, 0);
    wr(DISPI_Y_OFFSET, 0);
    wr(DISPI_ENABLE, DISPI_ENABLED | DISPI_LFB);
    if (rd(DISPI_XRES) != t->hactive || rd(DISPI_YRES) != t->vactive) return E_IO;   /* the adapter refused the mode */
    out->vaddr = b->fb;
    out->phys = b->fb_phys;
    out->w = t->hactive;
    out->h = t->vactive;
    out->pitch = t->hactive * 4;
    out->swap = false;
    return 0;
}

/* QEMU puts the monitor's EDID at the start of BAR 2 when the adapter is created with edid=on */
static int bochs_read_edid(struct gpu *g, uint8_t *buf, size_t max)
{
    struct bochs *b = g->priv;
    if (!b->mmio || max < 128) return 0;
    const uint8_t *e = b->mmio;
    static const uint8_t magic[8] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };
    if (memcmp(e, magic, 8)) return 0;
    size_t n = 128u * (1u + e[126]);
    if (n > max) n = max & ~127u;
    for (size_t i = 0; i < n; i++) buf[i] = e[i];
    return (int)n;
}

static const struct gpu_ops bochs_ops = {
    .set_mode = bochs_set_mode,
    .read_edid = bochs_read_edid,
};

static int bochs_init(void)
{
    struct pci_dev *p = NULL;
    for (int i = 0; i < pci_count(); i++) {
        struct pci_dev *d = pci_get(i);
        if (d->cls == 0x03 && ((d->vendor == 0x1234 && d->device == 0x1111) || (d->vendor == 0x80EE && d->device == 0xBEEF))) { p = d; break; }
    }
    if (!p) return E_NOENT;
    if ((rd(DISPI_ID) & 0xFFF0) != DISPI_ID_MIN) return E_NOENT;       /* not a dispi adapter after all */
    dev = kzalloc(sizeof(*dev));
    if (!dev) return E_NOMEM;
    dev->pci = p;
    dev->fb_phys = pci_bar_addr(p, 0);
    dev->vram = (uint64_t)rd(DISPI_VIDEO_MEMORY_64K) * 65536;
    if (!dev->vram) dev->vram = 16u << 20;
    uint64_t mmio = pci_bar_addr(p, 2);
    if (mmio && !pci_bar_is_io(p, 2)) dev->mmio = vmm_map_mmio(mmio, 0x1000);
    pci_write16(p->bus, p->dev, p->fn, 0x04, pci_read16(p->bus, p->dev, p->fn, 0x04) | 0x7);
    struct gpu *g = &dev->gpu;
    strlcpy(g->name, "Bochs/QEMU standard VGA", sizeof(g->name));
    strlcpy(g->driver, "bochs", sizeof(g->driver));
    g->vendor = p->vendor;
    g->device = p->device;
    g->pci = p;
    g->ops = &bochs_ops;
    g->priv = dev;
    g->max_w = 4096;
    g->max_h = 4096;
    return gpu_register(g);
}

static void bochs_exit(void)
{
    if (dev) { gpu_unregister(&dev->gpu); kfree(dev); dev = NULL; }
}

ZENITH_MODULE("bochs", "1.0", "Bochs/QEMU standard VGA display adapter", bochs_init, bochs_exit);

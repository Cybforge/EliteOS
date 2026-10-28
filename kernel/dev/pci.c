/* PCI bus enumeration via configuration mechanism #1. */
#include <kernel.h>
#include <dev.h>
#include <x86.h>
#include <mm.h>
#include <vfs.h>

#define MAX_PCI 128

static struct pci_dev devs[MAX_PCI];
static int ndevs;

static uint32_t addr(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    return 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) | ((uint32_t)fn << 8) | (off & 0xFC);
}

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    outl(0xCF8, addr(bus, dev, fn, off));
    return inl(0xCFC);
}

void pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v)
{
    outl(0xCF8, addr(bus, dev, fn, off));
    outl(0xCFC, v);
}

uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    return (uint16_t)(pci_read32(bus, dev, fn, off) >> ((off & 2) * 8));
}

void pci_write16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint16_t v)
{
    uint32_t cur = pci_read32(bus, dev, fn, off);
    int sh = (off & 2) * 8;
    cur = (cur & ~(0xFFFFu << sh)) | ((uint32_t)v << sh);
    pci_write32(bus, dev, fn, off, cur);
}

void pci_enable_busmaster(struct pci_dev *d)
{
    uint16_t cmd = pci_read16(d->bus, d->dev, d->fn, 0x04);
    cmd |= 0x0007;          /* I/O space + memory space + bus master */
    cmd &= (uint16_t)~0x0400;  /* interrupts enabled */
    pci_write16(d->bus, d->dev, d->fn, 0x04, cmd);
}

/* offset of the next capability with this id after `after` (0 = from the start), 0 if none */
uint8_t pci_find_cap(struct pci_dev *d, uint8_t id, uint8_t after)
{
    if (!(pci_read16(d->bus, d->dev, d->fn, 0x06) & 0x10)) return 0;
    uint8_t off = after ? (uint8_t)(pci_read32(d->bus, d->dev, d->fn, after) >> 8) : (uint8_t)pci_read32(d->bus, d->dev, d->fn, 0x34);
    for (int guard = 0; off >= 0x40 && guard < 48; guard++) {
        uint32_t v = pci_read32(d->bus, d->dev, d->fn, off & 0xFC);
        if ((v & 0xFF) == id) return off & 0xFC;
        off = (uint8_t)(v >> 8);
    }
    return 0;
}

uint64_t pci_bar_addr(struct pci_dev *d, int bar)
{
    uint32_t v = d->bar[bar];
    if (v & 1) return v & ~3u;                      /* I/O */
    uint64_t a = v & ~0xFu;
    if (((v >> 1) & 3) == 2 && bar < 5) a |= (uint64_t)d->bar[bar + 1] << 32;
    return a;
}

static void probe(uint8_t bus, uint8_t dev, uint8_t fn)
{
    uint32_t id = pci_read32(bus, dev, fn, 0);
    if ((id & 0xFFFF) == 0xFFFF || ndevs >= MAX_PCI) return;
    struct pci_dev *d = &devs[ndevs++];
    d->bus = bus; d->dev = dev; d->fn = fn;
    d->vendor = id & 0xFFFF;
    d->device = id >> 16;
    uint32_t cls = pci_read32(bus, dev, fn, 0x08);
    d->rev = cls & 0xFF;
    d->progif = (cls >> 8) & 0xFF;
    d->subcls = (cls >> 16) & 0xFF;
    d->cls = cls >> 24;
    uint32_t irq = pci_read32(bus, dev, fn, 0x3C);
    d->irq_line = irq & 0xFF;
    d->irq_pin = (irq >> 8) & 0xFF;
    uint8_t htype = (pci_read32(bus, dev, fn, 0x0C) >> 16) & 0x7F;
    if (htype == 0)
        for (int i = 0; i < 6; i++) d->bar[i] = pci_read32(bus, dev, fn, (uint8_t)(0x10 + i * 4));
}

void pci_init(void)
{
    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            uint32_t id = pci_read32((uint8_t)bus, (uint8_t)dev, 0, 0);
            if ((id & 0xFFFF) == 0xFFFF) continue;
            probe((uint8_t)bus, (uint8_t)dev, 0);
            uint8_t ht = (pci_read32((uint8_t)bus, (uint8_t)dev, 0, 0x0C) >> 16) & 0xFF;
            if (ht & 0x80)
                for (int fn = 1; fn < 8; fn++) probe((uint8_t)bus, (uint8_t)dev, (uint8_t)fn);
        }
    }
    klog("pci: %d devices", ndevs);
    for (int i = 0; i < ndevs; i++) {
        struct pci_dev *d = &devs[i];
        klog("pci: %02x:%02x.%x %04x:%04x %s", d->bus, d->dev, d->fn, d->vendor, d->device,
             pci_class_name(d->cls, d->subcls));
    }
}

int pci_count(void) { return ndevs; }
struct pci_dev *pci_get(int i) { return i >= 0 && i < ndevs ? &devs[i] : NULL; }

struct pci_dev *pci_find_class(uint8_t cls, uint8_t sub)
{
    for (int i = 0; i < ndevs; i++)
        if (devs[i].cls == cls && devs[i].subcls == sub) return &devs[i];
    return NULL;
}

struct pci_dev *pci_find(uint16_t vendor, uint16_t device)
{
    for (int i = 0; i < ndevs; i++)
        if (devs[i].vendor == vendor && devs[i].device == device) return &devs[i];
    return NULL;
}

const char *pci_class_name(uint8_t c, uint8_t s)
{
    switch (c) {
    case 0x00: return "Unclassified";
    case 0x01:
        switch (s) {
        case 0x01: return "IDE controller";
        case 0x06: return "SATA controller";
        case 0x08: return "NVMe controller";
        default: return "Storage controller";
        }
    case 0x02: return s == 0x80 ? "Network controller" : "Ethernet controller";
    case 0x03: return "Display controller";
    case 0x04: return s == 0x03 ? "Audio device" : "Multimedia controller";
    case 0x05: return "Memory controller";
    case 0x06:
        switch (s) {
        case 0x00: return "Host bridge";
        case 0x01: return "ISA bridge";
        case 0x04: return "PCI bridge";
        default: return "Bridge";
        }
    case 0x07: return "Communication controller";
    case 0x08: return "System peripheral";
    case 0x09: return "Input device";
    case 0x0C:
        switch (s) {
        case 0x03: return "USB controller";
        case 0x05: return "SMBus controller";
        default: return "Serial bus controller";
        }
    case 0x0D: return "Wireless controller";
    default: return "Device";
    }
}

/* vendor and device names come from /usr/share/misc/pci.ids (same format as the public pci.ids) */
static char *ids;
static bool ids_tried;

static void ids_load(void)
{
    if (ids_tried) return;
    ids_tried = true;
    size_t n;
    ids = vfs_read_file("/usr/share/misc/pci.ids", &n);
}

/* the line that starts at p, copied without its id into one of a few static buffers */
static const char *line_text(const char *p)
{
    static char buf[4][96];
    static int next;
    char *b = buf[next++ & 3];
    p += 4;
    while (*p == ' ') p++;
    size_t i = 0;
    while (*p && *p != '\n' && i + 1 < sizeof(buf[0])) b[i++] = *p++;
    b[i] = 0;
    return b;
}

static bool id_at(const char *p, unsigned id)
{
    for (int k = 0; k < 4; k++) {
        int c = (unsigned char)p[k], v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (v < 0 || v != (int)((id >> (12 - 4 * k)) & 15)) return false;
    }
    return p[4] == ' ' || p[4] == '\t';
}

static const char *find_vendor_line(uint16_t v)
{
    ids_load();
    if (!ids) return NULL;
    for (const char *p = ids; *p;) {
        if (*p != '#' && *p != '\t' && *p != '\n' && id_at(p, v)) return p;
        p = strchr(p, '\n');
        if (!p) break;
        p++;
    }
    return NULL;
}

const char *pci_vendor_name(uint16_t v)
{
    const char *l = find_vendor_line(v);
    return l ? line_text(l) : "Unknown vendor";
}

const char *pci_device_name(uint16_t v, uint16_t d)
{
    const char *l = find_vendor_line(v);
    if (!l) return NULL;
    for (l = strchr(l, '\n'); l && *++l == '\t';) {
        if (id_at(l + 1, d)) return line_text(l + 1);
        l = strchr(l, '\n');
    }
    return NULL;
}

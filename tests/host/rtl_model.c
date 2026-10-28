/*
 * The Realtek driver against a software model of the chip's register
 * interface: reset, descriptor rings with OWN/EOR/FS/LS, the transmit poll
 * register, PHYAR, link status. It checks what a real chip would depend on:
 * ring base addresses, ownership hand-over, wrap-around, error frames,
 * short-frame padding and recovery from a stalled transmitter.
 */
#include "kernel.h"
#include "netdev.h"
#include <stdarg.h>
#include <assert.h>

extern const struct nic_driver drv_r8169;

/* ---- the model ---- */
static uint8_t reg[256];
static uint16_t phy[32];
static struct { uint32_t opts1, opts2; uint64_t addr; } *txr, *rxr;
static unsigned model_tx, model_rx, resets;
static bool tx_blocked, locked = true, link_up = true;
static uint8_t wire[400][1600]; static size_t wire_len[400]; static int nwire;
static uint64_t tx_base, rx_base;
static bool unlocked_when_programming_failed;

#define OWN (1u << 31)
#define EOR (1u << 30)
#define FS (1u << 29)
#define LS (1u << 28)

void klog(const char *fmt, ...) { (void)fmt; }
void udelay(uint64_t us) { (void)us; }
void mdelay(uint64_t ms) { (void)ms; }
void pci_enable_busmaster(struct pci_dev *d) { (void)d; }
uint64_t pci_bar_addr(struct pci_dev *d, int bar) { return d->bar[bar] & ~0xFu; }
uint8_t pci_find_cap(struct pci_dev *d, uint8_t id, uint8_t after) { (void)d; (void)after; return id == 0x10 ? 0x70 : 0; }
static uint16_t linkctl = 0x0103;                           /* ASPM L0s+L1 and clock PM enabled */
uint16_t pci_read16(uint8_t b, uint8_t d, uint8_t f, uint8_t off) { (void)b; (void)d; (void)f; return off == 0x80 ? linkctl : 0; }
void pci_write16(uint8_t b, uint8_t d, uint8_t f, uint8_t off, uint16_t v) { (void)b; (void)d; (void)f; if (off == 0x80) linkctl = v; }
void *vmm_map_mmio(uint64_t phys, uint64_t size) { (void)phys; (void)size; return (void *)reg; }
void *dma_alloc(size_t bytes, uint64_t *phys)
{
    void *p;
    if (posix_memalign(&p, 4096, (bytes + 4095) & ~4095ul)) return NULL;
    memset(p, 0, bytes);
    if (phys) *phys = (uint64_t)(uintptr_t)p;
    return p;
}

static void model_tx_run(void)
{
    if (tx_blocked) return;
    for (;;) {
        if (!(txr[model_tx].opts1 & OWN)) break;
        uint32_t o = txr[model_tx].opts1;
        assert((o & FS) && (o & LS));                       /* the driver sends whole frames */
        size_t len = o & 0xFFFF;
        memcpy(wire[nwire], (void *)(uintptr_t)txr[model_tx].addr, len);
        wire_len[nwire++] = len;
        txr[model_tx].opts1 &= ~OWN;
        bool eor = o & EOR;
        model_tx = eor ? 0 : model_tx + 1;
    }
}

uint32_t model_read(uint32_t off, int size)
{
    if (off == 0x5C || off == 0x60) { /* PHYAR */ }
    uint32_t v = 0;
    memcpy(&v, reg + off, size);
    if (off == 0x6C) return link_up ? 0x03 : 0x00;           /* PHYstatus: link + full duplex */
    return v;
}

void model_write(uint32_t off, int size, uint32_t v)
{
    if (off == 0x37) {                                       /* ChipCmd */
        if (v & 0x10) { resets++; memset(reg, 0, 0x37); reg[0] = 0x52; reg[1] = 0x54; reg[2] = 0x00; reg[3] = 0x12; reg[4] = 0x34; reg[5] = 0x57; model_tx = model_rx = 0; reg[0x40 + 3] = 0x4C; v &= ~0x10; }
        reg[off] = (uint8_t)v;
        return;
    }
    if (off == 0x38 && (v & 0x40)) { model_tx_run(); return; }  /* TxPoll: normal priority queue */
    if (off == 0x50) { locked = (v & 0xC0) != 0xC0; }
    if (off == 0x60) {                                       /* PHYAR */
        uint32_t reg_no = (v >> 16) & 31;
        if (v & (1u << 31)) { phy[reg_no] = (uint16_t)v; v &= ~(1u << 31); }          /* write completes: flag clears */
        else v = (v & 0x1F0000) | phy[reg_no] | (1u << 31);                             /* read completes: flag sets */
    }
    /* configuration registers may only be written while unlocked */
    if ((off == 0xDA || off == 0xEC || (off >= 0x08 && off < 0x10 && size == 4)) && locked) unlocked_when_programming_failed = true;
    memcpy(reg + off, &v, size);
    if (off == 0x20 + 4 || off == 0x20) { memcpy(&tx_base, reg + 0x20, 8); txr = (void *)(uintptr_t)tx_base; }
    if (off == 0xE4 + 4 || off == 0xE4) { memcpy(&rx_base, reg + 0xE4, 8); rxr = (void *)(uintptr_t)rx_base; model_rx = 0; model_tx = 0; }
}

static bool inject(const void *frame, size_t len, uint32_t extra)
{
    if (!(rxr[model_rx].opts1 & OWN)) return false;
    memcpy((void *)(uintptr_t)rxr[model_rx].addr, frame, len);
    bool eor = rxr[model_rx].opts1 & EOR;
    rxr[model_rx].opts1 = (uint32_t)(len + 4) | FS | LS | extra | (eor ? EOR : 0);      /* the chip counts the CRC */
    model_rx = eor ? 0 : model_rx + 1;
    return true;
}

static int fails;
#define CHECK(c, msg) do { if (c) printf("ok   %s\n", msg); else { printf("FAIL %s\n", msg); fails++; } } while (0)

int main(void)
{
    struct pci_dev pd = { .vendor = 0x10EC, .device = 0x8168, .bar = { 0, 0, 0xF0000000u } };
    memset(reg, 0, sizeof(reg));
    phy[0] = 0x1140;
    reg[0] = 0x52; reg[1] = 0x54; reg[2] = 0x00; reg[3] = 0x12; reg[4] = 0x34; reg[5] = 0x56;
    struct netdev *nd = drv_r8169.attach(&pd, &drv_r8169.ids[0]);
    CHECK(nd != NULL, "attach");
    if (!nd) return 1;
    CHECK(!memcmp(nd->mac, "\x52\x54\x00\x12\x34\x56", 6) || !memcmp(nd->mac, "\x52\x54\x00\x12\x34\x57", 6), "MAC address read from the ID registers");
    CHECK(strstr(nd->name, "Realtek PCIe GbE Family Controller") != NULL, "named after the family");
    CHECK(tx_base && !(tx_base & 255) && rx_base && !(rx_base & 255), "ring base addresses are programmed and 256-byte aligned");
    CHECK(!unlocked_when_programming_failed, "configuration registers written only while unlocked");
    CHECK(locked, "configuration registers locked again at the end");
    CHECK(reg[0x37] == 0x0C, "receiver and transmitter enabled");
    CHECK((linkctl & 0x0103) == 0, "PCIe ASPM and clock power management switched off");
    CHECK(phy[0] & (1 << 9), "auto-negotiation restarted");
    CHECK(nd->link(nd), "link up reported from PHYstatus");
    link_up = false; CHECK(!nd->link(nd), "link down reported"); link_up = true;

    /* transmit: short frame is padded, more frames than ring slots wrap around */
    uint8_t f[1500];
    for (int i = 0; i < 1500; i++) f[i] = (uint8_t)i;
    CHECK(nd->send(nd, f, 20), "send a short frame");
    CHECK(nwire == 1 && wire_len[0] == 60 && !memcmp(wire[0], f, 20) && !wire[0][59], "short frame padded to 60 bytes");
    int sent_ok = 1;
    for (int i = 0; i < 300; i++) {
        f[0] = (uint8_t)i;
        if (!nd->send(nd, f, 100 + (size_t)i)) sent_ok = 0;
    }
    CHECK(sent_ok && nwire == 301, "301 frames through a 64-slot ring (wrap-around)");
    int content_ok = 1;
    for (int i = 0; i < 300; i++)
        if (wire_len[i + 1] != 100 + (size_t)i || wire[i + 1][0] != (uint8_t)i || wire[i + 1][50] != 50) content_ok = 0;
    CHECK(content_ok, "frame contents and order on the wire");

    /* receive: many frames, with a damaged one and a split one in between */
    uint8_t out[2048];
    int got = 0, expect = 0;
    for (int round = 0; round < 3; round++) {
        for (int i = 0; i < 100; i++) {
            uint8_t fr[200]; memset(fr, 0, sizeof(fr));
            fr[0] = (uint8_t)(round * 100 + i); memset(fr + 1, (uint8_t)i, 150);
            if (i == 10) { CHECK(inject(fr, 150, 1u << 21), "inject a frame with RES set"); continue; }
            if (i == 20) {                                    /* first descriptor of a frame that spans two */
                inject(fr, 150, 0); rxr[(model_rx + 127) % 128].opts1 &= ~LS; continue;
            }
            if (!inject(fr, 150, 0)) { CHECK(0, "ring never ran dry"); break; }
            expect++;
        }
        int n;
        while ((n = nd->poll(nd, out, sizeof(out))) > 0) {
            if (n != 150) CHECK(0, "received length excludes the CRC");
            got++;
        }
    }
    CHECK(got == expect, "all good frames received across ring wraps, damaged and split ones dropped");

    /* a transmitter that stops: after three failed sends the driver resets the chip and starts over */
    unsigned r0 = resets;
    tx_blocked = true;
    int refused = 0;
    for (int i = 0; i < 70; i++) if (!nd->send(nd, f, 64)) refused++;
    CHECK(refused > 0, "sends are refused while the ring is full");
    CHECK(resets > r0, "a stalled transmitter makes the driver reset the chip");
    tx_blocked = false;
    int before = nwire;
    f[0] = 0xEE;
    CHECK(nd->send(nd, f, 64), "sending works again after the reset");
    CHECK(nwire > before && wire[nwire - 1][0] == 0xEE, "the frame after the reset reaches the wire");

    printf("%d failures\n", fails);
    return fails != 0;
}

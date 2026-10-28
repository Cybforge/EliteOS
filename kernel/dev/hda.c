/*
 * Intel High Definition Audio controller and codec driver.
 *
 * Talks to codecs through the CORB/RIRB rings (falling back to the
 * immediate command registers), finds every output pin that leads to a
 * DAC, unmutes the path and plays one output stream: a DMA ring buffer of
 * 48 kHz 16-bit stereo that the mixer (audio.c) keeps filled.
 */
#include <audio.h>
#include <dev.h>
#include <mm.h>
#include <x86.h>
#include <vfs.h>

#define GCAP      0x00
#define GCTL      0x08
#define STATESTS  0x0E
#define INTCTL    0x20
#define CORBLBASE 0x40
#define CORBUBASE 0x44
#define CORBWP    0x48
#define CORBRP    0x4A
#define CORBCTL   0x4C
#define CORBSIZE  0x4E
#define RIRBLBASE 0x50
#define RIRBUBASE 0x54
#define RIRBWP    0x58
#define RINTCNT   0x5A
#define RIRBCTL   0x5C
#define RIRBSTS   0x5D
#define RIRBSIZE  0x5E
#define ICOI      0x60
#define ICII      0x64
#define ICIS      0x68

#define SD_CTL    0x00
#define SD_STS    0x03
#define SD_LPIB   0x04
#define SD_CBL    0x08
#define SD_LVI    0x0C
#define SD_FMT    0x12
#define SD_BDPL   0x18
#define SD_BDPU   0x1C

#define RING_FRAMES 8192
#define NBDL        8
#define STREAM_TAG  1
#define FMT_48K_16_STEREO 0x0011

static volatile uint8_t *mmio;
static uint32_t *corb;
static uint64_t *rirb;
static uint16_t corb_wp, rirb_rp;
static bool use_immediate;
static bool present;
static int codec = -1;
static volatile uint8_t *sd;           /* output stream descriptor */
static int16_t *ring;
static char name[64];
static int ndacs, npins;

static uint8_t  r8(uint32_t r) { return *(volatile uint8_t *)(mmio + r); }
static uint16_t r16(uint32_t r) { return *(volatile uint16_t *)(mmio + r); }
static uint32_t r32(uint32_t r) { return *(volatile uint32_t *)(mmio + r); }
static void w8(uint32_t r, uint8_t v) { *(volatile uint8_t *)(mmio + r) = v; }
static void w16(uint32_t r, uint16_t v) { *(volatile uint16_t *)(mmio + r) = v; }
static void w32(uint32_t r, uint32_t v) { *(volatile uint32_t *)(mmio + r) = v; }

static bool wait_bits(uint32_t reg, int width, uint32_t mask, uint32_t want, int ms)
{
    uint64_t end = uptime_ms() + (uint64_t)ms;
    for (;;) {
        uint32_t v = width == 8 ? r8(reg) : width == 16 ? r16(reg) : r32(reg);
        if ((v & mask) == want) return true;
        if (uptime_ms() > end) return false;
        cpu_relax();
    }
}

/* ------------------------------------------------------------------------
 * codec commands
 * ---------------------------------------------------------------------- */

static bool cmd_immediate(uint32_t verb, uint32_t *resp)
{
    if (!wait_bits(ICIS, 16, 1, 0, 50)) return false;
    w16(ICIS, 2);                       /* clear "result valid" */
    w32(ICOI, verb);
    w16(ICIS, 1);                       /* busy */
    if (!wait_bits(ICIS, 16, 2, 2, 50)) return false;
    *resp = r32(ICII);
    return true;
}

static uint32_t cmd(uint32_t verb)
{
    uint32_t resp = 0;
    if (use_immediate) {
        cmd_immediate(verb, &resp);
        return resp;
    }
    corb_wp = (uint16_t)((corb_wp + 1) % 256);
    corb[corb_wp] = verb;
    w16(CORBWP, corb_wp);
    uint64_t end = uptime_ms() + 50;
    while ((r16(RIRBWP) & 0xFF) == rirb_rp) {
        if (uptime_ms() > end) {
            /* the rings do not work here: use the immediate interface from now on */
            klog("hda: CORB/RIRB timeout (corb rp %x wp %x ctl %x, rirb wp %x ctl %x sts %x), using immediate commands",
                 r16(CORBRP), r16(CORBWP), r8(CORBCTL), r16(RIRBWP), r8(RIRBCTL), r8(RIRBSTS));
            use_immediate = true;
            cmd_immediate(verb, &resp);
            return resp;
        }
        cpu_relax();
    }
    rirb_rp = (uint16_t)((rirb_rp + 1) % 256);
    resp = (uint32_t)rirb[rirb_rp];
    w8(RIRBSTS, 0x05);
    return resp;
}

static uint32_t verb12(int nid, uint32_t v, uint32_t payload)
{
    return cmd(((uint32_t)codec << 28) | ((uint32_t)nid << 20) | (v << 8) | (payload & 0xFF));
}

static uint32_t verb4(int nid, uint32_t v, uint32_t payload)
{
    return cmd(((uint32_t)codec << 28) | ((uint32_t)nid << 20) | (v << 16) | (payload & 0xFFFF));
}

static uint32_t param(int nid, int p) { return verb12(nid, 0xF00, (uint32_t)p); }

/* ------------------------------------------------------------------------
 * finding the output paths
 * ---------------------------------------------------------------------- */

#define MAX_NODES 128
static uint32_t caps[MAX_NODES];
static int first_nid, nnodes, afg;

/* amplifier capabilities live in the widget or, unless overridden, in the function group */
static uint32_t amp_caps(int nid, int which)
{
    return param(caps[nid] & (1u << 3) ? nid : afg, which);
}

static int wtype(int nid) { return (int)((caps[nid] >> 20) & 0xF); }

static int conn_list(int nid, int *out, int max)
{
    if (!(caps[nid] & (1u << 8))) return 0;
    uint32_t len = param(nid, 0x0E);
    bool lng = len & 0x80;
    int n = (int)(len & 0x7F), k = 0;
    for (int i = 0; i < n && k < max; i += lng ? 2 : 4) {
        uint32_t r = verb12(nid, 0xF02, (uint32_t)i);
        for (int j = 0; j < (lng ? 2 : 4) && i + j < n && k < max; j++) {
            uint32_t e = lng ? (r >> (16 * j)) & 0xFFFF : (r >> (8 * j)) & 0xFF;
            uint32_t mask = lng ? 0x7FFF : 0x7F;
            bool range = lng ? (e & 0x8000) : (e & 0x80);
            e &= mask;
            if (range && k > 0) {
                for (int v = out[k - 1] + 1; v <= (int)e && k < max; v++) out[k++] = v;
            } else {
                out[k++] = (int)e;
            }
        }
    }
    return k;
}

static void unmute_out(int nid)
{
    if (caps[nid] & (1u << 2)) {
        uint32_t gain = amp_caps(nid, 0x12) & 0x7F;      /* the 0 dB step */
        verb4(nid, 0x3, 0xB000 | gain);      /* output, left+right, unmute */
    }
}

static void unmute_in(int nid, int index)
{
    if (caps[nid] & (1u << 1)) {
        uint32_t gain = amp_caps(nid, 0x0D) & 0x7F;
        verb4(nid, 0x3, 0x7000 | ((uint32_t)index << 8) | gain);   /* input, left+right */
    }
}

/* depth-first search from a pin to a DAC; configures the path on success */
static int route(int nid, int depth)
{
    if (nid < first_nid || nid >= first_nid + nnodes || depth > 6) return 0;
    int t = wtype(nid);
    if (t == 0) {                                  /* audio output = DAC */
        verb12(nid, 0x705, 0);                     /* power D0 */
        verb4(nid, 0x2, FMT_48K_16_STEREO);
        verb12(nid, 0x706, (STREAM_TAG << 4) | 0);
        unmute_out(nid);
        return nid;
    }
    if (t != 2 && t != 3 && t != 4) return 0;      /* mixer, selector, pin */
    int list[32];
    int n = conn_list(nid, list, 32);
    for (int i = 0; i < n; i++) {
        int dac = route(list[i], depth + 1);
        if (!dac) continue;
        verb12(nid, 0x705, 0);
        if (t != 2 && n > 1) verb12(nid, 0x701, (uint32_t)i);   /* select that input */
        unmute_in(nid, i);
        unmute_out(nid);
        return dac;
    }
    return 0;
}

static bool setup_codec(int cad)
{
    codec = cad;
    uint32_t vendor = param(0, 0x00);
    if (!vendor || vendor == 0xFFFFFFFF) return false;
    uint32_t sub = param(0, 0x04);
    int fg_start = (int)((sub >> 16) & 0xFF), fg_count = (int)(sub & 0xFF);
    for (int fg = fg_start; fg < fg_start + fg_count; fg++) {
        if ((param(fg, 0x05) & 0xFF) != 1) continue;      /* audio function group */
        afg = fg;
        verb12(fg, 0x705, 0);                             /* power up */
        mdelay(10);
        uint32_t ws = param(fg, 0x04);
        first_nid = (int)((ws >> 16) & 0xFF);
        nnodes = (int)(ws & 0xFF);
        if (first_nid + nnodes > MAX_NODES) nnodes = MAX_NODES - first_nid;
        for (int n = first_nid; n < first_nid + nnodes; n++) caps[n] = param(n, 0x09);

        /* every usable output pin: speakers, headphones, line outs */
        for (int n = first_nid; n < first_nid + nnodes; n++) {
            if (wtype(n) != 4) continue;
            uint32_t pc = param(n, 0x0C);
            if (!(pc & (1u << 4))) continue;              /* no output capability */
            uint32_t cfg = verb12(n, 0xF1C, 0);
            int conn = (int)(cfg >> 30), dev = (int)((cfg >> 20) & 0xF);
            if (conn == 1) continue;                      /* nothing attached */
            if (dev != 0 && dev != 1 && dev != 2 && dev != 0xF && cfg) continue;
            int dac = route(n, 0);
            if (!dac) continue;
            uint32_t ctl = 0x40 | (dev == 2 ? 0x80 : 0);  /* out enable, HP amp for headphones */
            verb12(n, 0x707, ctl);
            if (pc & (1u << 16)) verb12(n, 0x70C, 0x02);  /* external amplifier (EAPD) on */
            npins++;
            ndacs++;
        }
        snprintf(name, sizeof(name), "HD Audio codec %04x:%04x", vendor >> 16, vendor & 0xFFFF);
        return npins > 0;
    }
    return false;
}

/* ------------------------------------------------------------------------
 * controller
 * ---------------------------------------------------------------------- */

static bool setup_rings(uint64_t page)
{
    corb = P2V(page);
    rirb = (uint64_t *)((uint8_t *)P2V(page) + 1024);
    w8(CORBCTL, 0);
    w8(RIRBCTL, 0);
    wait_bits(CORBCTL, 8, 2, 0, 20);
    wait_bits(RIRBCTL, 8, 2, 0, 20);
    w8(CORBSIZE, (r8(CORBSIZE) & 0xF0) | 2);          /* 256 entries */
    w8(RIRBSIZE, (r8(RIRBSIZE) & 0xF0) | 2);
    w32(CORBLBASE, (uint32_t)page);
    w32(CORBUBASE, (uint32_t)(page >> 32));
    w32(RIRBLBASE, (uint32_t)(page + 1024));
    w32(RIRBUBASE, (uint32_t)((page + 1024) >> 32));
    /* reset the CORB read pointer (some controllers never acknowledge; that's fine) */
    w16(CORBRP, 0x8000);
    wait_bits(CORBRP, 16, 0x8000, 0x8000, 5);
    w16(CORBRP, 0);
    wait_bits(CORBRP, 16, 0x8000, 0, 5);
    w16(CORBWP, 0);
    w16(RIRBWP, 0x8000);
    w16(RINTCNT, 1);
    corb_wp = 0;
    rirb_rp = 0;
    w8(CORBCTL, 2);
    /* DMA on, plus the response interrupt flag: the controller counts responses
     * and only continues once that flag is acknowledged (INTCTL stays off) */
    w8(RIRBCTL, 3);
    return true;
}

static bool setup_stream(int iss)
{
    sd = mmio + 0x80 + (uint32_t)iss * 0x20;
    *(volatile uint8_t *)(sd + SD_CTL) = 0;
    /* stream reset */
    *(volatile uint8_t *)(sd + SD_CTL) = 1;
    for (int i = 0; i < 1000 && !(*(volatile uint8_t *)(sd + SD_CTL) & 1); i++) udelay(10);
    *(volatile uint8_t *)(sd + SD_CTL) = 0;
    for (int i = 0; i < 1000 && (*(volatile uint8_t *)(sd + SD_CTL) & 1); i++) udelay(10);

    size_t bytes = RING_FRAMES * 4;
    uint64_t buf = pmm_alloc_contig(bytes / PAGE_SIZE);
    uint64_t bdl_page = pmm_alloc_below(0x100000000ull);
    if (!buf || !bdl_page) return false;
    ring = P2V(buf);
    memset(ring, 0, bytes);
    struct { uint64_t addr; uint32_t len, ioc; } *bdl = P2V(bdl_page);
    for (int i = 0; i < NBDL; i++) {
        bdl[i].addr = buf + (uint64_t)i * (bytes / NBDL);
        bdl[i].len = (uint32_t)(bytes / NBDL);
        bdl[i].ioc = 0;
    }
    *(volatile uint32_t *)(sd + SD_CBL) = (uint32_t)bytes;
    *(volatile uint16_t *)(sd + SD_LVI) = NBDL - 1;
    *(volatile uint16_t *)(sd + SD_FMT) = FMT_48K_16_STEREO;
    *(volatile uint32_t *)(sd + SD_BDPL) = (uint32_t)bdl_page;
    *(volatile uint32_t *)(sd + SD_BDPU) = (uint32_t)(bdl_page >> 32);
    /* stream tag in bits 20..23, then run */
    volatile uint32_t *ctl = (volatile uint32_t *)(sd + SD_CTL);
    *ctl = (*ctl & 0xFF0FFFFF) | ((uint32_t)STREAM_TAG << 20);
    *(volatile uint8_t *)(sd + SD_STS) = 0x1C;
    *ctl |= 2;
    return true;
}

bool hda_init(void)
{
    struct pci_dev *pd = NULL;
    for (int i = 0; i < pci_count(); i++) {
        struct pci_dev *d = pci_get(i);
        if (d->cls == 0x04 && d->subcls == 0x03) { pd = d; break; }
    }
    if (!pd) return false;
    uint64_t bar = pci_bar_addr(pd, 0);
    if (!bar) return false;
    pci_enable_busmaster(pd);
    pci_write16(pd->bus, pd->dev, pd->fn, 0x04, (uint16_t)(pci_read16(pd->bus, pd->dev, pd->fn, 0x04) | 0x06));
    mmio = vmm_map_mmio(bar, 0x4000);

    /* controller reset */
    w32(GCTL, r32(GCTL) & ~1u);
    wait_bits(GCTL, 32, 1, 0, 100);
    udelay(100);
    w32(GCTL, r32(GCTL) | 1);
    if (!wait_bits(GCTL, 32, 1, 1, 100)) { klog("hda: controller did not leave reset"); return false; }
    mdelay(2);                                        /* codecs announce themselves */
    w32(INTCTL, 0);
    uint16_t gcap = r16(GCAP);
    int iss = (gcap >> 8) & 0xF, oss = (gcap >> 12) & 0xF;
    if (!oss) { klog("hda: no output streams"); return false; }

    uint64_t page = pmm_alloc_below(0x100000000ull);
    setup_rings(page);
    uint16_t codecs = r16(STATESTS);
    if (!codecs) codecs = 1;                          /* some controllers don't report; try codec 0 */
    bool ok = false;
    for (int c = 0; c < 15 && !ok; c++)
        if (codecs & (1u << c)) ok = setup_codec(c);
    if (!ok) { klog("hda: no codec with a usable output"); return false; }
    if (!setup_stream(iss)) return false;
    present = true;
    klog("hda: %04x:%04x at %02x:%02x.%x, %s, %d output pin%s", pd->vendor, pd->device, pd->bus, pd->dev, pd->fn, name,
         npins, npins == 1 ? "" : "s");
    return true;
}

bool hda_present(void) { return present; }
const char *hda_name(void) { return present ? name : "none"; }

uint32_t hda_play_pos(void)
{
    if (!present) return 0;
    return (*(volatile uint32_t *)(sd + SD_LPIB) / 4) % RING_FRAMES;
}

int16_t *hda_buffer(uint32_t *frames)
{
    *frames = RING_FRAMES;
    return ring;
}

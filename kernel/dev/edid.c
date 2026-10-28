/*
 * Monitor identification: parses EDID (base block, CTA-861 and DisplayID
 * extensions) into a list of modes with their timings, and computes CVT
 * reduced-blanking timings for modes the monitor only names.
 */
#include <kernel.h>
#include <gpu.h>

/* ---- CVT reduced blanking, version 1 ---- */

void gpu_cvt_rb(int w, int h, int hz, struct gpu_timing *t)
{
    int vsync;
    if (w * 3 == h * 4) vsync = 4;
    else if (w * 9 == h * 16) vsync = 5;
    else if (w * 10 == h * 16) vsync = 6;
    else if (w * 4 == h * 5) vsync = 7;
    else if (w * 9 == h * 15) vsync = 7;
    else vsync = 10;
    uint64_t frame_ns = 1000000000ull / (uint64_t)hz;
    uint64_t line_ns = (frame_ns - 460000) / (uint64_t)h;         /* the blanking interval takes at least 460 us */
    int vbi = (int)(460000 / line_ns) + 1;
    if (vbi < 3 + vsync + 6) vbi = 3 + vsync + 6;
    int htotal = w + 160, vtotal = h + vbi;
    uint64_t pclk = (uint64_t)hz * (uint64_t)htotal * (uint64_t)vtotal / 1000;
    pclk -= pclk % 250;                                           /* the pixel clock is a multiple of 0.25 MHz */
    memset(t, 0, sizeof(*t));
    t->pclk_khz = (uint32_t)pclk;
    t->hactive = (uint16_t)w;
    t->hfront = 48;
    t->hsync = 32;
    t->hback = 80;
    t->vactive = (uint16_t)h;
    t->vfront = 3;
    t->vsync = (uint16_t)vsync;
    t->vback = (uint16_t)(vbi - 3 - vsync);
    t->hsync_pos = true;
    t->vsync_pos = false;
    t->refresh_mhz = (uint32_t)(pclk * 1000000ull / ((uint64_t)htotal * (uint64_t)vtotal));
}

/* ---- collecting modes ---- */

static void add_mode(struct edid_info *e, const struct gpu_timing *t)
{
    if (t->hactive < 640 || t->vactive < 480) return;
    for (int i = 0; i < e->nmodes; i++) {
        const struct gpu_timing *o = &e->modes[i];
        int d = (int)o->refresh_mhz - (int)t->refresh_mhz;
        if (o->hactive == t->hactive && o->vactive == t->vactive && d > -600 && d < 600) {
            if (t->from_edid_timing && !o->from_edid_timing) e->modes[i] = *t;     /* an exact timing beats a computed one */
            return;
        }
    }
    if (e->nmodes < EDID_MAX_MODES) e->modes[e->nmodes++] = *t;
}

static void add_named(struct edid_info *e, int w, int h, int hz)
{
    struct gpu_timing t;
    gpu_cvt_rb(w, h, hz, &t);
    add_mode(e, &t);
}

/* an 18-byte detailed timing descriptor; returns false if it is not one */
static bool parse_dtd(const uint8_t *d, struct gpu_timing *t)
{
    uint32_t pclk = (uint32_t)(d[0] | d[1] << 8) * 10;           /* kHz */
    if (!pclk) return false;
    if (d[17] & 0x80) return false;                               /* interlaced */
    memset(t, 0, sizeof(*t));
    t->pclk_khz = pclk;
    t->hactive = (uint16_t)(d[2] | (d[4] & 0xF0) << 4);
    int hblank = d[3] | (d[4] & 0x0F) << 8;
    t->vactive = (uint16_t)(d[5] | (d[7] & 0xF0) << 4);
    int vblank = d[6] | (d[7] & 0x0F) << 8;
    t->hfront = (uint16_t)(d[8] | (d[11] & 0xC0) << 2);
    t->hsync = (uint16_t)(d[9] | (d[11] & 0x30) << 4);
    t->vfront = (uint16_t)((d[10] >> 4) | (d[11] & 0x0C) << 2);
    t->vsync = (uint16_t)((d[10] & 0x0F) | (d[11] & 0x03) << 4);
    t->hback = (uint16_t)(hblank - t->hfront - t->hsync);
    t->vback = (uint16_t)(vblank - t->vfront - t->vsync);
    t->hsync_pos = d[17] & 0x02;
    t->vsync_pos = d[17] & 0x04;
    if (!t->hactive || !t->vactive || hblank < t->hfront + t->hsync || vblank < t->vfront + t->vsync) return false;
    uint64_t total = (uint64_t)gpu_htotal(t) * (uint64_t)gpu_vtotal(t);
    t->refresh_mhz = (uint32_t)((uint64_t)pclk * 1000000ull / total);
    t->from_edid_timing = true;
    return true;
}

/* CTA-861 video identification codes (progressive modes) */
static const struct { uint8_t vic; uint16_t w, h; uint16_t hz; } vics[] = {
    { 1, 640, 480, 60 },   { 2, 720, 480, 60 },   { 3, 720, 480, 60 },   { 4, 1280, 720, 60 },
    { 16, 1920, 1080, 60 }, { 17, 720, 576, 50 },  { 18, 720, 576, 50 },  { 19, 1280, 720, 50 },
    { 31, 1920, 1080, 50 }, { 32, 1920, 1080, 24 }, { 33, 1920, 1080, 25 }, { 34, 1920, 1080, 30 },
    { 41, 1280, 720, 100 }, { 47, 1280, 720, 120 }, { 60, 1280, 720, 24 }, { 61, 1280, 720, 25 },
    { 62, 1280, 720, 30 }, { 63, 1920, 1080, 120 }, { 64, 1920, 1080, 100 }, { 93, 3840, 2160, 24 },
    { 94, 3840, 2160, 25 }, { 95, 3840, 2160, 30 }, { 96, 3840, 2160, 50 }, { 97, 3840, 2160, 60 },
    { 98, 4096, 2160, 24 }, { 99, 4096, 2160, 25 }, { 100, 4096, 2160, 30 }, { 101, 4096, 2160, 50 },
    { 102, 4096, 2160, 60 }, { 117, 3840, 2160, 100 }, { 118, 3840, 2160, 120 },
};

static void parse_cta(const uint8_t *b, struct edid_info *e)
{
    int dtd = b[2];
    if (dtd < 4 || dtd > 127) return;
    for (int i = 4; i < dtd;) {
        int tag = b[i] >> 5, len = b[i] & 0x1F;
        if (i + 1 + len > dtd) break;
        if (tag == 2) {                                           /* video data block */
            for (int k = 0; k < len; k++) {
                int v = b[i + 1 + k];
                int vic = (v <= 64 || (v >= 129 && v <= 192)) ? (v & 0x7F) : v;
                for (unsigned m = 0; m < ARRAY_SIZE(vics); m++)
                    if (vics[m].vic == vic) add_named(e, vics[m].w, vics[m].h, vics[m].hz);
            }
        }
        i += 1 + len;
    }
    for (int i = dtd; i + 18 <= 127; i += 18) {
        struct gpu_timing t;
        if (b[i] == 0 && b[i + 1] == 0) break;
        if (parse_dtd(b + i, &t)) add_mode(e, &t);
    }
}

static void parse_displayid(const uint8_t *b, struct edid_info *e)
{
    int size = b[2];
    int end = 5 + size;
    if (end > 127) end = 127;
    for (int i = 5; i + 3 <= end;) {
        int tag = b[i], len = b[i + 2];
        if (!tag && !len) break;
        if (i + 3 + len > end + 1) break;
        if (tag == 0x03) {                                        /* type I detailed timings, 20 bytes each */
            for (int p = i + 3; p + 20 <= i + 3 + len; p += 20) {
                const uint8_t *d = b + p;
                struct gpu_timing t;
                memset(&t, 0, sizeof(t));
                if (d[3] & 0x10) continue;                        /* interlaced */
                t.pclk_khz = ((uint32_t)(d[0] | d[1] << 8 | d[2] << 16) + 1) * 10;
                t.hactive = (uint16_t)((d[4] | d[5] << 8) + 1);
                int htotal = (d[6] | d[7] << 8) + 1;
                t.hfront = (uint16_t)(((d[8] | d[9] << 8) & 0x7FFF) + 1);
                t.hsync_pos = d[9] & 0x80;
                t.hsync = (uint16_t)((d[10] | d[11] << 8) + 1);
                t.vactive = (uint16_t)((d[12] | d[13] << 8) + 1);
                int vtotal = (d[14] | d[15] << 8) + 1;
                t.vfront = (uint16_t)(((d[16] | d[17] << 8) & 0x7FFF) + 1);
                t.vsync_pos = d[17] & 0x80;
                t.vsync = (uint16_t)((d[18] | d[19] << 8) + 1);
                if (htotal < t.hactive + t.hfront + t.hsync || vtotal < t.vactive + t.vfront + t.vsync) continue;
                t.hback = (uint16_t)(htotal - t.hactive - t.hfront - t.hsync);
                t.vback = (uint16_t)(vtotal - t.vactive - t.vfront - t.vsync);
                t.refresh_mhz = (uint32_t)((uint64_t)t.pclk_khz * 1000000ull / ((uint64_t)htotal * (uint64_t)vtotal));
                t.from_edid_timing = true;
                add_mode(e, &t);
            }
        }
        i += 3 + len;
    }
}

/* the old fixed lists of the base block: established and standard timings */
static void parse_legacy(const uint8_t *b, struct edid_info *e)
{
    static const struct { uint8_t byte, bit; uint16_t w, h, hz; } est[] = {
        { 35, 5, 640, 480, 60 }, { 35, 3, 800, 600, 60 }, { 35, 0, 800, 600, 56 }, { 36, 3, 1024, 768, 60 },
        { 36, 0, 1024, 768, 70 }, { 36, 1, 1024, 768, 75 }, { 37, 7, 1280, 1024, 75 },
    };
    for (unsigned i = 0; i < ARRAY_SIZE(est); i++)
        if (b[est[i].byte] & (1u << est[i].bit)) add_named(e, est[i].w, est[i].h, est[i].hz);
    for (int i = 38; i < 54; i += 2) {
        if (b[i] == 0x01 && b[i + 1] == 0x01) continue;
        int w = (b[i] + 31) * 8, h = 0;
        switch (b[i + 1] >> 6) {
        case 0: h = w * 10 / 16; break;
        case 1: h = w * 3 / 4; break;
        case 2: h = w * 4 / 5; break;
        default: h = w * 9 / 16; break;
        }
        int hz = (b[i + 1] & 0x3F) + 60;
        add_named(e, w, h, hz);
    }
}

bool edid_parse(const uint8_t *b, size_t n, struct edid_info *e)
{
    static const uint8_t magic[8] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };
    memset(e, 0, sizeof(*e));
    e->preferred = -1;
    if (n < 128 || memcmp(b, magic, 8)) return false;
    uint8_t sum = 0;
    for (int i = 0; i < 128; i++) sum = (uint8_t)(sum + b[i]);
    if (sum) return false;
    e->valid = true;
    uint16_t m = (uint16_t)(b[8] << 8 | b[9]);
    e->mfg[0] = (char)('A' + ((m >> 10) & 31) - 1);
    e->mfg[1] = (char)('A' + ((m >> 5) & 31) - 1);
    e->mfg[2] = (char)('A' + (m & 31) - 1);
    e->product = (uint16_t)(b[10] | b[11] << 8);
    e->width_mm = b[21] * 10;
    e->height_mm = b[22] * 10;

    /* detailed timings and descriptors; the first timing is the preferred one */
    for (int i = 54; i < 126; i += 18) {
        const uint8_t *d = b + i;
        struct gpu_timing t;
        if (d[0] || d[1]) {
            if (parse_dtd(d, &t)) {
                add_mode(e, &t);
                if (e->preferred < 0) {
                    for (int k = 0; k < e->nmodes; k++)
                        if (e->modes[k].hactive == t.hactive && e->modes[k].vactive == t.vactive &&
                            e->modes[k].refresh_mhz == t.refresh_mhz) { e->preferred = k; break; }
                }
            }
        } else if (d[3] == 0xFC) {                                /* monitor name */
            int l = 0;
            for (int k = 5; k < 18 && d[k] != '\n' && d[k]; k++) e->name[l++] = (char)d[k];
            e->name[l] = 0;
        } else if (d[3] == 0xFD) {                                /* range limits */
            int vo = d[4] & 3, ho = (d[4] >> 2) & 3;                  /* EDID 1.4 offsets for rates above 255 */
            e->vmin = d[5] + (vo == 3 ? 255 : 0);
            e->vmax = d[6] + (vo >= 2 ? 255 : 0);
            e->hmin = d[7] + (ho == 3 ? 255 : 0);
            e->hmax = d[8] + (ho >= 2 ? 255 : 0);
            e->max_pclk_mhz = d[9] * 10;
        }
    }
    parse_legacy(b, e);
    int ext = b[126];
    if ((size_t)(ext + 1) * 128 > n) ext = (int)(n / 128) - 1;
    for (int i = 1; i <= ext; i++) {
        const uint8_t *x = b + (size_t)i * 128;
        uint8_t s = 0;
        for (int k = 0; k < 128; k++) s = (uint8_t)(s + x[k]);
        if (s) continue;
        if (x[0] == 0x02) parse_cta(x, e);
        else if (x[0] == 0x70) parse_displayid(x, e);
    }
    return e->nmodes > 0;
}

/* ---- choosing ---- */

static bool mode_ok(const struct edid_info *e, const struct gpu *g, const struct gpu_timing *t)
{
    if (g->max_w && t->hactive > g->max_w) return false;
    if (g->max_h && t->vactive > g->max_h) return false;
    if (g->max_pclk_khz && t->pclk_khz > g->max_pclk_khz) return false;
    if (e->max_pclk_mhz && t->pclk_khz > (uint32_t)e->max_pclk_mhz * 1000 + 10000) return false;
    int hz = (int)((t->refresh_mhz + 500) / 1000);
    if (e->vmax && !t->from_edid_timing && (hz < e->vmin || hz > e->vmax)) return false;
    return true;
}

/* the native resolution at the highest refresh rate the monitor and the adapter both manage */
bool gpu_pick_mode(const struct edid_info *e, const struct gpu *g, struct gpu_timing *out)
{
    if (!e || !e->valid || !e->nmodes) return false;
    int nw = 0, nh = 0;
    if (e->preferred >= 0) { nw = e->modes[e->preferred].hactive; nh = e->modes[e->preferred].vactive; }
    int best = -1;
    /* first the native size, then, if the adapter cannot do it, the largest size it can */
    for (int pass = 0; pass < 2 && best < 0; pass++) {
        long best_area = 0;
        for (int i = 0; i < e->nmodes; i++) {
            const struct gpu_timing *t = &e->modes[i];
            if (!mode_ok(e, g, t)) continue;
            long area = (long)t->hactive * t->vactive;
            if (pass == 0) {
                if (!nw || t->hactive != nw || t->vactive != nh) continue;
                if (best < 0 || t->refresh_mhz > e->modes[best].refresh_mhz) best = i;
            } else if (area > best_area || (area == best_area && t->refresh_mhz > e->modes[best].refresh_mhz)) {
                best = i;
                best_area = area;
            }
        }
    }
    if (best < 0) return false;
    *out = e->modes[best];
    return true;
}

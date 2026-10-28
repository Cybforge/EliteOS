/*
 * Image decoders: PNG (all colour types and bit depths, Adam7 interlacing),
 * baseline JPEG (Huffman, 4:4:4 / 4:2:2 / 4:2:0 / greyscale, restart
 * markers) and BMP (24/32-bit). Everything decodes into a 32-bit surface.
 */
#include <gfx.h>
#include <mm.h>
#include <vfs.h>
#include <inflate.h>
#include <image.h>

#define MAX_DIM 8192

/* ========================================================================
 * PNG
 * ====================================================================== */

static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

static int paeth(int a, int b, int c)
{
    int p = a + b - c, pa = ABS(p - a), pb = ABS(p - b), pc = ABS(p - c);
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}

/* undo the per-row filters of one (sub)image in place; rows are 1 + rowbytes long */
static bool unfilter(uint8_t *d, int rows, size_t rowbytes, int bpp)
{
    uint8_t *prev = NULL;
    for (int y = 0; y < rows; y++) {
        uint8_t *row = d + (size_t)y * (rowbytes + 1);
        int f = row[0];
        uint8_t *px = row + 1;
        for (size_t i = 0; i < rowbytes; i++) {
            int a = i >= (size_t)bpp ? px[i - bpp] : 0;
            int b = prev ? prev[i] : 0;
            int c = prev && i >= (size_t)bpp ? prev[i - bpp] : 0;
            switch (f) {
            case 0: break;
            case 1: px[i] = (uint8_t)(px[i] + a); break;
            case 2: px[i] = (uint8_t)(px[i] + b); break;
            case 3: px[i] = (uint8_t)(px[i] + ((a + b) >> 1)); break;
            case 4: px[i] = (uint8_t)(px[i] + paeth(a, b, c)); break;
            default: return false;
            }
        }
        prev = px;
    }
    return true;
}

struct png_info {
    int w, h, depth, ctype, channels;
    uint8_t pal[256][4];
    int npal;
    bool has_trns;
    uint16_t trns[3];
};

static uint32_t sample(const uint8_t *row, int x, int depth, int ch, int c)
{
    if (depth == 8) return row[(size_t)x * ch + c];
    if (depth == 16) return row[((size_t)x * ch + c) * 2];     /* high byte */
    /* 1, 2, 4 bits: single channel only */
    int per = 8 / depth;
    int shift = (per - 1 - x % per) * depth;
    return (row[x / per] >> shift) & ((1u << depth) - 1);
}

static color_t png_pixel(const struct png_info *pi, const uint8_t *row, int x)
{
    int d = pi->depth, ch = pi->channels;
    switch (pi->ctype) {
    case 0: {
        uint32_t v = sample(row, x, d, 1, 0);
        uint32_t raw16 = d == 16 ? ((uint32_t)row[x * 2] << 8 | row[x * 2 + 1]) : v;
        if (d < 8) v = v * 255 / ((1u << d) - 1);
        uint8_t a = pi->has_trns && raw16 == pi->trns[0] ? 0 : 255;
        return RGBA(v, v, v, a);
    }
    case 2: {
        uint32_t r = sample(row, x, d, ch, 0), g = sample(row, x, d, ch, 1), b = sample(row, x, d, ch, 2);
        uint8_t a = 255;
        if (pi->has_trns && d == 8 && r == pi->trns[0] && g == pi->trns[1] && b == pi->trns[2]) a = 0;
        return RGBA(r, g, b, a);
    }
    case 3: {
        uint32_t i = sample(row, x, d, 1, 0);
        if ((int)i >= pi->npal) return 0;
        return RGBA(pi->pal[i][0], pi->pal[i][1], pi->pal[i][2], pi->pal[i][3]);
    }
    case 4: {
        uint32_t v = sample(row, x, d, ch, 0), a = sample(row, x, d, ch, 1);
        return RGBA(v, v, v, a);
    }
    default:
        return RGBA(sample(row, x, d, ch, 0), sample(row, x, d, ch, 1), sample(row, x, d, ch, 2), sample(row, x, d, ch, 3));
    }
}

static surface_t *png_decode(const uint8_t *d, size_t n)
{
    if (n < 33 || memcmp(d, "\x89PNG\r\n\x1a\n", 8)) return NULL;
    struct png_info pi = { 0 };
    int interlace = 0;
    size_t idat_len = 0, idat_cap = 0;
    uint8_t *idat = NULL;
    for (size_t off = 8; off + 12 <= n;) {
        uint32_t len = be32(d + off);
        const uint8_t *type = d + off + 4, *body = d + off + 8;
        if (off + 12 + len > n) break;
        if (!memcmp(type, "IHDR", 4) && len >= 13) {
            pi.w = (int)be32(body);
            pi.h = (int)be32(body + 4);
            pi.depth = body[8];
            pi.ctype = body[9];
            interlace = body[12];
        } else if (!memcmp(type, "PLTE", 4)) {
            pi.npal = (int)MIN(len / 3, 256u);
            for (int i = 0; i < pi.npal; i++) {
                pi.pal[i][0] = body[i * 3];
                pi.pal[i][1] = body[i * 3 + 1];
                pi.pal[i][2] = body[i * 3 + 2];
                pi.pal[i][3] = 255;
            }
        } else if (!memcmp(type, "tRNS", 4)) {
            pi.has_trns = true;
            if (pi.ctype == 3) {
                for (uint32_t i = 0; i < len && i < 256; i++) pi.pal[i][3] = body[i];
            } else {
                for (int i = 0; i < 3 && (uint32_t)(i * 2 + 1) < len; i++) pi.trns[i] = (uint16_t)(body[i * 2] << 8 | body[i * 2 + 1]);
            }
        } else if (!memcmp(type, "IDAT", 4)) {
            if (idat_len + len > idat_cap) {
                idat_cap = MAX(idat_cap * 2, idat_len + len + 4096);
                uint8_t *nb = krealloc(idat, idat_cap);
                if (!nb) { kfree(idat); return NULL; }
                idat = nb;
            }
            memcpy(idat + idat_len, body, len);
            idat_len += len;
        } else if (!memcmp(type, "IEND", 4)) {
            break;
        }
        off += 12 + len;
    }
    static const int chans[7] = { 1, 0, 3, 1, 2, 0, 4 };
    if (!idat || pi.w <= 0 || pi.h <= 0 || pi.w > MAX_DIM || pi.h > MAX_DIM || pi.ctype > 6 || !chans[pi.ctype] ||
        (pi.depth != 1 && pi.depth != 2 && pi.depth != 4 && pi.depth != 8 && pi.depth != 16)) {
        kfree(idat);
        return NULL;
    }
    pi.channels = chans[pi.ctype];
    int bits_pp = pi.channels * pi.depth;
    int bpp = MAX(1, bits_pp / 8);

    /* the size of the decompressed stream, including the Adam7 passes */
    static const int ax0[7] = { 0, 4, 0, 2, 0, 1, 0 }, ay0[7] = { 0, 0, 4, 0, 2, 0, 1 };
    static const int adx[7] = { 8, 8, 4, 4, 2, 2, 1 }, ady[7] = { 8, 8, 8, 4, 4, 2, 2 };
    int passes = interlace ? 7 : 1;
    size_t total = 0;
    int pw[7], ph[7];
    for (int p = 0; p < passes; p++) {
        pw[p] = interlace ? (pi.w - ax0[p] + adx[p] - 1) / adx[p] : pi.w;
        ph[p] = interlace ? (pi.h - ay0[p] + ady[p] - 1) / ady[p] : pi.h;
        if (pw[p] <= 0 || ph[p] <= 0) continue;
        total += (size_t)ph[p] * (1 + ((size_t)pw[p] * bits_pp + 7) / 8);
    }
    uint8_t *raw = kmalloc(total + 16);
    size_t got = 0;
    if (!raw || zlib_inflate(idat, idat_len, raw, total, &got) || got < total) {
        kfree(idat);
        kfree(raw);
        return NULL;
    }
    kfree(idat);
    surface_t *s = surface_new(pi.w, pi.h);
    if (!s) { kfree(raw); return NULL; }
    size_t off = 0;
    for (int p = 0; p < passes; p++) {
        if (pw[p] <= 0 || ph[p] <= 0) continue;
        size_t rowbytes = ((size_t)pw[p] * bits_pp + 7) / 8;
        if (!unfilter(raw + off, ph[p], rowbytes, bpp)) { surface_free(s); kfree(raw); return NULL; }
        for (int y = 0; y < ph[p]; y++) {
            const uint8_t *row = raw + off + (size_t)y * (rowbytes + 1) + 1;
            int dy = interlace ? ay0[p] + y * ady[p] : y;
            for (int x = 0; x < pw[p]; x++) {
                int dx = interlace ? ax0[p] + x * adx[p] : x;
                s->px[(size_t)dy * s->stride + dx] = png_pixel(&pi, row, x);
            }
        }
        off += (size_t)ph[p] * (rowbytes + 1);
    }
    kfree(raw);
    return s;
}

/* ========================================================================
 * JPEG (baseline)
 * ====================================================================== */

struct jhuff {
    uint8_t bits[17];
    uint8_t vals[256];
    int mincode[17], maxcode[18], valptr[17];
};

struct jcomp {
    int id, h, v, tq, td, ta;
    int bw, bh;               /* blocks per line / column in this component */
    uint8_t *plane;
    int stride;
    int dc;
};

struct jpeg {
    const uint8_t *p, *end;
    uint32_t bitbuf;
    int bitcnt;
    bool err;
    uint16_t q[4][64];
    struct jhuff dc[4], ac[4];
    struct jcomp c[3];
    int nc, w, h, hmax, vmax, restart;
    bool progressive;
};

static const uint8_t zigzag[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

static void jhuff_build(struct jhuff *h)
{
    int code = 0, k = 0;
    for (int l = 1; l <= 16; l++) {
        h->valptr[l] = k;
        h->mincode[l] = code;
        code += h->bits[l];
        k += h->bits[l];
        h->maxcode[l] = h->bits[l] ? code - 1 : -1;
        code <<= 1;
    }
    h->maxcode[17] = 0x7FFFFFFF;
}

static int jbit(struct jpeg *j)
{
    if (!j->bitcnt) {
        if (j->p >= j->end) { j->err = true; return 0; }
        uint8_t b = *j->p++;
        if (b == 0xFF) {
            uint8_t nx = j->p < j->end ? *j->p : 0;
            if (nx == 0) j->p++;                       /* stuffed zero byte */
            else { j->p--; b = 0; j->err = nx < 0xD0 || nx > 0xD7; if (!j->err) b = 0; }
        }
        j->bitbuf = b;
        j->bitcnt = 8;
    }
    j->bitcnt--;
    return (int)((j->bitbuf >> j->bitcnt) & 1);
}

static int jbits(struct jpeg *j, int n)
{
    int v = 0;
    while (n--) v = (v << 1) | jbit(j);
    return v;
}

static int jdecode(struct jpeg *j, const struct jhuff *h)
{
    int code = jbit(j);
    int l = 1;
    while (l <= 16 && code > h->maxcode[l]) {
        code = (code << 1) | jbit(j);
        l++;
    }
    if (l > 16) { j->err = true; return 0; }
    return h->vals[(h->valptr[l] + code - h->mincode[l]) & 255];
}

static int extend(int v, int t) { return v < (1 << (t - 1)) ? v - (1 << t) + 1 : v; }

static float idct_cos[8][8];

static void idct_init(void)
{
    static bool ready;
    if (ready) return;
    for (int x = 0; x < 8; x++)
        for (int u = 0; u < 8; u++) {
            float cu = u ? 1.0f : 0.70710678f;
            idct_cos[x][u] = cu * (float)k_cos((2 * x + 1) * u * 3.14159265358979 / 16.0) / 2.0f;
        }
    ready = true;
}

static void idct8x8(const int *in, uint8_t *out, int stride)
{
    float tmp[64];
    for (int y = 0; y < 8; y++)                /* rows: frequency v fixed per row of input */
        for (int x = 0; x < 8; x++) {
            float s = 0;
            for (int u = 0; u < 8; u++) s += idct_cos[x][u] * (float)in[y * 8 + u];
            tmp[y * 8 + x] = s;
        }
    for (int x = 0; x < 8; x++)
        for (int y = 0; y < 8; y++) {
            float s = 0;
            for (int v = 0; v < 8; v++) s += idct_cos[y][v] * tmp[v * 8 + x];
            int val = (int)(s + 128.5f);
            out[y * stride + x] = (uint8_t)CLAMP(val, 0, 255);
        }
}

static bool jpeg_block(struct jpeg *j, struct jcomp *c, int bx, int by)
{
    int coef[64] = { 0 };
    int t = jdecode(j, &j->dc[c->td]);
    int diff = t ? extend(jbits(j, t), t) : 0;
    c->dc += diff;
    coef[0] = c->dc * j->q[c->tq][0];
    for (int k = 1; k < 64;) {
        int rs = jdecode(j, &j->ac[c->ta]);
        int r = rs >> 4, s = rs & 15;
        if (!s) {
            if (r != 15) break;                    /* end of block */
            k += 16;
            continue;
        }
        k += r;
        if (k > 63) return false;
        coef[zigzag[k]] = extend(jbits(j, s), s) * j->q[c->tq][k];
        k++;
    }
    if (j->err) return false;
    if (bx * 8 < c->stride && by * 8 * c->stride < c->stride * c->bh * 8)
        idct8x8(coef, c->plane + (size_t)by * 8 * c->stride + bx * 8, c->stride);
    return true;
}

/* a component sample at full-resolution pixel (x, y); subsampled chroma is
 * interpolated bilinearly between the sample centres */
static int jsample(const struct jpeg *j, const struct jcomp *c, int x, int y)
{
    if (c->h == j->hmax && c->v == j->vmax) return c->plane[(size_t)y * c->stride + x];
    int fx = ((2 * x + 1) * c->h * 128) / j->hmax - 128;     /* in 1/256 sample units */
    int fy = ((2 * y + 1) * c->v * 128) / j->vmax - 128;
    int maxx = c->bw * 8 - 1, maxy = c->bh * 8 - 1;
    int x0 = fx >> 8, y0 = fy >> 8, ax = fx & 255, ay = fy & 255;
    if (fx < 0) { x0 = 0; ax = 0; }
    if (fy < 0) { y0 = 0; ay = 0; }
    int x1 = MIN(x0 + 1, maxx), y1 = MIN(y0 + 1, maxy);
    x0 = MIN(x0, maxx);
    y0 = MIN(y0, maxy);
    const uint8_t *r0 = c->plane + (size_t)y0 * c->stride, *r1 = c->plane + (size_t)y1 * c->stride;
    int top = r0[x0] * (256 - ax) + r0[x1] * ax;
    int bot = r1[x0] * (256 - ax) + r1[x1] * ax;
    return (top * (256 - ay) + bot * ay + 32768) >> 16;
}

static surface_t *jpeg_decode(const uint8_t *d, size_t n)
{
    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) return NULL;
    struct jpeg *j = kzalloc(sizeof(*j));
    if (!j) return NULL;
    idct_init();
    surface_t *s = NULL;
    size_t i = 2;
    bool frame = false;
    while (i + 4 <= n) {
        if (d[i] != 0xFF) { i++; continue; }
        uint8_t m = d[i + 1];
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7) || m == 0xFF) { i += m == 0xFF ? 1 : 2; continue; }
        if (m == 0xD9) break;
        size_t len = (size_t)(d[i + 2] << 8 | d[i + 3]);
        const uint8_t *b = d + i + 4;
        if (i + 2 + len > n || len < 2) break;
        size_t blen = len - 2;
        if (m == 0xDB) {                                    /* quantization tables */
            for (size_t o = 0; o < blen;) {
                int pq = b[o] >> 4, tq = b[o] & 3;
                o++;
                for (int k = 0; k < 64; k++) {
                    j->q[tq][k] = pq ? (uint16_t)(b[o] << 8 | b[o + 1]) : b[o];
                    o += pq ? 2 : 1;
                }
            }
        } else if (m == 0xC4) {                             /* Huffman tables */
            for (size_t o = 0; o + 17 <= blen;) {
                int tc = b[o] >> 4, th = b[o] & 3;
                struct jhuff *h = tc ? &j->ac[th] : &j->dc[th];
                int total = 0;
                for (int l = 1; l <= 16; l++) { h->bits[l] = b[o + l]; total += b[o + l]; }
                o += 17;
                if (total > 256 || o + (size_t)total > blen) break;
                memcpy(h->vals, b + o, (size_t)total);
                o += (size_t)total;
                jhuff_build(h);
            }
        } else if (m == 0xDD) {
            j->restart = b[0] << 8 | b[1];
        } else if (m == 0xC0 || m == 0xC1) {                /* baseline / extended sequential */
            j->h = b[1] << 8 | b[2];
            j->w = b[3] << 8 | b[4];
            j->nc = b[5];
            if (j->nc != 1 && j->nc != 3) break;
            if (j->w <= 0 || j->h <= 0 || j->w > MAX_DIM || j->h > MAX_DIM) break;
            for (int k = 0; k < j->nc; k++) {
                j->c[k].id = b[6 + k * 3];
                j->c[k].h = b[7 + k * 3] >> 4;
                j->c[k].v = b[7 + k * 3] & 15;
                j->c[k].tq = b[8 + k * 3] & 3;
                j->hmax = MAX(j->hmax, j->c[k].h);
                j->vmax = MAX(j->vmax, j->c[k].v);
            }
            frame = true;
        } else if (m == 0xC2 || m == 0xC3 || (m >= 0xC5 && m <= 0xCF && m != 0xC8 && m != 0xCC)) {
            j->progressive = true;                          /* progressive / lossless / arithmetic */
            break;
        } else if (m == 0xDA && frame) {                    /* start of scan: decode the image data */
            int ns = b[0];
            for (int k = 0; k < ns; k++) {
                int id = b[1 + k * 2], t = b[2 + k * 2];
                for (int q = 0; q < j->nc; q++)
                    if (j->c[q].id == id) { j->c[q].td = t >> 4; j->c[q].ta = t & 15; }
            }
            if (!j->hmax || !j->vmax) break;
            int mcuw = 8 * j->hmax, mcuh = 8 * j->vmax;
            int mx = (j->w + mcuw - 1) / mcuw, my = (j->h + mcuh - 1) / mcuh;
            for (int k = 0; k < j->nc; k++) {
                struct jcomp *c = &j->c[k];
                c->bw = mx * c->h;
                c->bh = my * c->v;
                c->stride = c->bw * 8;
                c->plane = kmalloc((size_t)c->stride * c->bh * 8);
                if (!c->plane) goto out;
            }
            j->p = b + blen;
            j->end = d + n;
            int until_restart = j->restart;
            for (int y = 0; y < my && !j->err; y++)
                for (int x = 0; x < mx && !j->err; x++) {
                    if (j->restart && !until_restart) {
                        /* align to the RSTn marker and reset the predictors */
                        j->bitcnt = 0;
                        while (j->p + 1 < j->end && !(j->p[0] == 0xFF && j->p[1] >= 0xD0 && j->p[1] <= 0xD7)) j->p++;
                        if (j->p + 1 < j->end) j->p += 2;
                        for (int k = 0; k < j->nc; k++) j->c[k].dc = 0;
                        until_restart = j->restart;
                    }
                    for (int k = 0; k < j->nc; k++) {
                        struct jcomp *c = &j->c[k];
                        for (int v = 0; v < c->v; v++)
                            for (int h = 0; h < c->h; h++)
                                if (!jpeg_block(j, c, x * c->h + h, y * c->v + v)) j->err = true;
                    }
                    if (j->restart) until_restart--;
                }
            if (j->err && j->p >= j->end) j->err = false;   /* truncated file: keep what we have */
            s = surface_new(j->w, j->h);
            if (!s) goto out;
            for (int y = 0; y < j->h; y++)
                for (int x = 0; x < j->w; x++) {
                    int Y = jsample(j, &j->c[0], x, y), cb = 128, cr = 128;
                    if (j->nc == 3) {
                        cb = jsample(j, &j->c[1], x, y);
                        cr = jsample(j, &j->c[2], x, y);
                    }
                    int r = Y + ((91881 * (cr - 128)) >> 16);
                    int g = Y - ((22554 * (cb - 128) + 46802 * (cr - 128)) >> 16);
                    int bl = Y + ((116130 * (cb - 128)) >> 16);
                    s->px[(size_t)y * s->stride + x] = RGB(CLAMP(r, 0, 255), CLAMP(g, 0, 255), CLAMP(bl, 0, 255));
                }
            break;
        }
        i += 2 + len;
    }
out:
    for (int k = 0; k < 3; k++) kfree(j->c[k].plane);
    kfree(j);
    return s;
}

/* ========================================================================
 * BMP
 * ====================================================================== */

static surface_t *bmp_decode(const uint8_t *d, size_t n)
{
    if (n <= 54 || d[0] != 'B' || d[1] != 'M') return NULL;
    uint32_t off = *(const uint32_t *)(d + 10);
    int32_t w = *(const int32_t *)(d + 18), h = *(const int32_t *)(d + 22);
    uint16_t bpp = *(const uint16_t *)(d + 28);
    bool topdown = h < 0;
    if (h < 0) h = -h;
    if (w <= 0 || h <= 0 || w > MAX_DIM || h > MAX_DIM || (bpp != 24 && bpp != 32)) return NULL;
    surface_t *s = surface_new(w, h);
    if (!s) return NULL;
    size_t stride = bpp == 24 ? (size_t)((w * 3 + 3) & ~3) : (size_t)w * 4;
    for (int y = 0; y < h; y++) {
        size_t row = topdown ? (size_t)y : (size_t)(h - 1 - y);
        if (off + row * stride + stride > n) break;
        const uint8_t *src = d + off + row * stride;
        for (int x = 0; x < w; x++) {
            const uint8_t *px = src + x * (bpp / 8);
            s->px[(size_t)y * s->stride + x] = RGB(px[2], px[1], px[0]);
        }
    }
    return s;
}

/* ======================================================================== */

surface_t *image_decode(const void *data, size_t n)
{
    const uint8_t *d = data;
    if (n >= 8 && d[0] == 0x89 && d[1] == 'P') return png_decode(d, n);
    if (n >= 4 && d[0] == 0xFF && d[1] == 0xD8) return jpeg_decode(d, n);
    if (n >= 2 && d[0] == 'B' && d[1] == 'M') return bmp_decode(d, n);
    return NULL;
}

surface_t *image_load(const char *path)
{
    size_t n;
    char *d = vfs_read_file(path, &n);
    if (!d) return NULL;
    surface_t *s = image_decode(d, n);
    kfree(d);
    return s;
}

bool image_is_supported(const char *name)
{
    size_t l = strlen(name);
    static const char *ext[] = { ".png", ".jpg", ".jpeg", ".bmp" };
    for (unsigned i = 0; i < ARRAY_SIZE(ext); i++) {
        size_t e = strlen(ext[i]);
        if (l > e && !strcasecmp(name + l - e, ext[i])) return true;
    }
    return false;
}

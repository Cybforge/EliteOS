/*
 * DEFLATE decompressor (RFC 1951) with zlib framing (RFC 1950), used by the
 * PNG decoder. Canonical Huffman decoding in the style of zlib's "puff".
 */
#include <kernel.h>
#include <mm.h>
#include <inflate.h>

struct bitreader {
    const uint8_t *p, *end;
    uint32_t buf;
    int cnt;
    bool err;
};

struct huff {
    uint16_t count[16];
    uint16_t symbol[320];
};

static uint32_t getbits(struct bitreader *b, int n)
{
    while (b->cnt < n) {
        if (b->p >= b->end) { b->err = true; return 0; }
        b->buf |= (uint32_t)*b->p++ << b->cnt;
        b->cnt += 8;
    }
    uint32_t v = b->buf & ((1u << n) - 1);
    b->buf >>= n;
    b->cnt -= n;
    return v;
}

static void build(struct huff *h, const uint8_t *lens, int n)
{
    uint16_t offs[16];
    memset(h->count, 0, sizeof(h->count));
    for (int i = 0; i < n; i++) h->count[lens[i]]++;
    h->count[0] = 0;
    offs[1] = 0;
    for (int i = 1; i < 15; i++) offs[i + 1] = (uint16_t)(offs[i] + h->count[i]);
    for (int i = 0; i < n; i++)
        if (lens[i]) h->symbol[offs[lens[i]]++] = (uint16_t)i;
}

static int decode(struct bitreader *b, const struct huff *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= (int)getbits(b, 1);
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
        if (b->err) return -1;
    }
    return -1;
}

static const uint16_t lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
                                    1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const uint8_t dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static int codes(struct bitreader *b, const struct huff *lit, const struct huff *dist, uint8_t *out, size_t cap, size_t *pos)
{
    for (;;) {
        int sym = decode(b, lit);
        if (sym < 0 || b->err) return -1;
        if (sym < 256) {
            if (*pos >= cap) return -2;
            out[(*pos)++] = (uint8_t)sym;
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29) return -1;
            size_t len = lbase[sym] + getbits(b, lext[sym]);
            int ds = decode(b, dist);
            if (ds < 0 || ds >= 30) return -1;
            size_t d = dbase[ds] + getbits(b, dext[ds]);
            if (d > *pos) return -1;
            if (*pos + len > cap) return -2;
            for (size_t i = 0; i < len; i++, (*pos)++) out[*pos] = out[*pos - d];
        }
    }
}

int inflate_raw(const uint8_t *src, size_t n, uint8_t *out, size_t cap, size_t *out_len)
{
    struct bitreader b = { src, src + n, 0, 0, false };
    struct huff *lit = kmalloc(sizeof(struct huff) * 3);
    if (!lit) return -3;
    struct huff *dist = lit + 1, *lens = lit + 2;
    size_t pos = 0;
    int last, r = 0;
    do {
        last = (int)getbits(&b, 1);
        int type = (int)getbits(&b, 2);
        if (b.err) { r = -1; break; }
        if (type == 0) {
            b.buf = 0;
            b.cnt = 0;
            if (b.p + 4 > b.end) { r = -1; break; }
            uint16_t len = (uint16_t)(b.p[0] | (b.p[1] << 8)), nlen = (uint16_t)(b.p[2] | (b.p[3] << 8));
            b.p += 4;
            if ((uint16_t)~nlen != len || b.p + len > b.end) { r = -1; break; }
            if (pos + len > cap) { r = -2; break; }
            memcpy(out + pos, b.p, len);
            b.p += len;
            pos += len;
        } else if (type == 1) {
            uint8_t l[320];
            int i = 0;
            for (; i < 144; i++) l[i] = 8;
            for (; i < 256; i++) l[i] = 9;
            for (; i < 280; i++) l[i] = 7;
            for (; i < 288; i++) l[i] = 8;
            build(lit, l, 288);
            for (i = 0; i < 30; i++) l[i] = 5;
            build(dist, l, 30);
            r = codes(&b, lit, dist, out, cap, &pos);
        } else if (type == 2) {
            static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
            int nlen = (int)getbits(&b, 5) + 257, ndist = (int)getbits(&b, 5) + 1, ncode = (int)getbits(&b, 4) + 4;
            if (nlen > 286 || ndist > 30) { r = -1; break; }
            uint8_t l[320] = { 0 };
            for (int i = 0; i < ncode; i++) l[order[i]] = (uint8_t)getbits(&b, 3);
            build(lens, l, 19);
            int i = 0;
            uint8_t all[320] = { 0 };
            while (i < nlen + ndist) {
                int sym = decode(&b, lens);
                if (sym < 0 || b.err) { r = -1; break; }
                if (sym < 16) { all[i++] = (uint8_t)sym; continue; }
                int rep, val = 0;
                if (sym == 16) {
                    if (!i) { r = -1; break; }
                    val = all[i - 1];
                    rep = 3 + (int)getbits(&b, 2);
                } else if (sym == 17) rep = 3 + (int)getbits(&b, 3);
                else rep = 11 + (int)getbits(&b, 7);
                if (i + rep > nlen + ndist) { r = -1; break; }
                while (rep--) all[i++] = (uint8_t)val;
            }
            if (r) break;
            build(lit, all, nlen);
            build(dist, all + nlen, ndist);
            r = codes(&b, lit, dist, out, cap, &pos);
        } else {
            r = -1;
        }
    } while (!last && !r);
    kfree(lit);
    *out_len = pos;
    return r;
}

int zlib_inflate(const uint8_t *src, size_t n, uint8_t *out, size_t cap, size_t *out_len)
{
    if (n < 2 || (src[0] & 0x0F) != 8 || ((src[0] << 8) | src[1]) % 31) return -1;
    if (src[1] & 0x20) return -1;               /* preset dictionary: not used by PNG */
    return inflate_raw(src + 2, n - 2, out, cap, out_len);
}

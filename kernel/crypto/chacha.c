/* ChaCha20, Poly1305 and the ChaCha20-Poly1305 AEAD (RFC 8439). */
#include <crypto.h>

static inline uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static inline void put_le32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static inline uint32_t rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

#define QR(a, b, c, d) \
    a += b; d ^= a; d = rotl(d, 16); c += d; b ^= c; b = rotl(b, 12); \
    a += b; d ^= a; d = rotl(d, 8);  c += d; b ^= c; b = rotl(b, 7);

void chacha20_block(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], uint8_t out[64])
{
    uint32_t s[16], x[16];
    s[0] = 0x61707865; s[1] = 0x3320646e; s[2] = 0x79622d32; s[3] = 0x6b206574;
    for (int i = 0; i < 8; i++) s[4 + i] = le32(key + 4 * i);
    s[12] = counter;
    for (int i = 0; i < 3; i++) s[13 + i] = le32(nonce + 4 * i);
    memcpy(x, s, sizeof(s));
    for (int i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8], x[12]) QR(x[1], x[5], x[9], x[13]) QR(x[2], x[6], x[10], x[14]) QR(x[3], x[7], x[11], x[15])
        QR(x[0], x[5], x[10], x[15]) QR(x[1], x[6], x[11], x[12]) QR(x[2], x[7], x[8], x[13]) QR(x[3], x[4], x[9], x[14])
    }
    for (int i = 0; i < 16; i++) put_le32(out + 4 * i, x[i] + s[i]);
}

void chacha20_xor(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], const uint8_t *in, uint8_t *out, size_t len)
{
    uint8_t ks[64];
    for (size_t off = 0; off < len; off += 64, counter++) {
        chacha20_block(key, counter, nonce, ks);
        size_t k = MIN(len - off, (size_t)64);
        for (size_t i = 0; i < k; i++) out[off + i] = in[off + i] ^ ks[i];
    }
}

/* ------------------------------------------------------------------------
 * Poly1305 with 26-bit limbs
 * ---------------------------------------------------------------------- */

struct poly {
    uint32_t r[5], h[5], pad[4];
    uint8_t buf[16];
    size_t n;
};

static void poly_init(struct poly *p, const uint8_t key[32])
{
    p->r[0] = le32(key + 0) & 0x3ffffff;
    p->r[1] = (le32(key + 3) >> 2) & 0x3ffff03;
    p->r[2] = (le32(key + 6) >> 4) & 0x3ffc0ff;
    p->r[3] = (le32(key + 9) >> 6) & 0x3f03fff;
    p->r[4] = (le32(key + 12) >> 8) & 0x00fffff;
    for (int i = 0; i < 5; i++) p->h[i] = 0;
    for (int i = 0; i < 4; i++) p->pad[i] = le32(key + 16 + 4 * i);
    p->n = 0;
}

static void poly_blocks(struct poly *p, const uint8_t *m, size_t len, uint32_t hibit)
{
    uint32_t r0 = p->r[0], r1 = p->r[1], r2 = p->r[2], r3 = p->r[3], r4 = p->r[4];
    uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
    uint32_t h0 = p->h[0], h1 = p->h[1], h2 = p->h[2], h3 = p->h[3], h4 = p->h[4];
    while (len >= 16) {
        h0 += le32(m + 0) & 0x3ffffff;
        h1 += (le32(m + 3) >> 2) & 0x3ffffff;
        h2 += (le32(m + 6) >> 4) & 0x3ffffff;
        h3 += (le32(m + 9) >> 6) & 0x3ffffff;
        h4 += (le32(m + 12) >> 8) | hibit;
        uint64_t d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4 + (uint64_t)h2 * s3 + (uint64_t)h3 * s2 + (uint64_t)h4 * s1;
        uint64_t d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 + (uint64_t)h2 * s4 + (uint64_t)h3 * s3 + (uint64_t)h4 * s2;
        uint64_t d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 + (uint64_t)h2 * r0 + (uint64_t)h3 * s4 + (uint64_t)h4 * s3;
        uint64_t d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 + (uint64_t)h2 * r1 + (uint64_t)h3 * r0 + (uint64_t)h4 * s4;
        uint64_t d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 + (uint64_t)h2 * r2 + (uint64_t)h3 * r1 + (uint64_t)h4 * r0;
        uint32_t c = (uint32_t)(d0 >> 26); h0 = (uint32_t)d0 & 0x3ffffff;
        d1 += c; c = (uint32_t)(d1 >> 26); h1 = (uint32_t)d1 & 0x3ffffff;
        d2 += c; c = (uint32_t)(d2 >> 26); h2 = (uint32_t)d2 & 0x3ffffff;
        d3 += c; c = (uint32_t)(d3 >> 26); h3 = (uint32_t)d3 & 0x3ffffff;
        d4 += c; c = (uint32_t)(d4 >> 26); h4 = (uint32_t)d4 & 0x3ffffff;
        h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff;
        h1 += c;
        m += 16;
        len -= 16;
    }
    p->h[0] = h0; p->h[1] = h1; p->h[2] = h2; p->h[3] = h3; p->h[4] = h4;
}

static void poly_update(struct poly *p, const uint8_t *m, size_t len)
{
    if (p->n) {
        size_t k = MIN(len, 16 - p->n);
        memcpy(p->buf + p->n, m, k);
        p->n += k;
        m += k;
        len -= k;
        if (p->n < 16) return;
        poly_blocks(p, p->buf, 16, 1u << 24);
        p->n = 0;
    }
    size_t full = len & ~(size_t)15;
    if (full) poly_blocks(p, m, full, 1u << 24);
    m += full;
    len -= full;
    if (len) {
        memcpy(p->buf, m, len);
        p->n = len;
    }
}

static void poly_final(struct poly *p, uint8_t tag[16])
{
    if (p->n) {
        p->buf[p->n] = 1;
        for (size_t i = p->n + 1; i < 16; i++) p->buf[i] = 0;
        poly_blocks(p, p->buf, 16, 0);
    }
    uint32_t h0 = p->h[0], h1 = p->h[1], h2 = p->h[2], h3 = p->h[3], h4 = p->h[4], c;
    c = h1 >> 26; h1 &= 0x3ffffff; h2 += c;
    c = h2 >> 26; h2 &= 0x3ffffff; h3 += c;
    c = h3 >> 26; h3 &= 0x3ffffff; h4 += c;
    c = h4 >> 26; h4 &= 0x3ffffff; h0 += c * 5;
    c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;
    uint32_t g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffff;
    uint32_t g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffff;
    uint32_t g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffff;
    uint32_t g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffff;
    uint32_t g4 = h4 + c - (1u << 26);
    uint32_t mask = (g4 >> 31) - 1;             /* all ones if h >= p */
    g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0; h1 = (h1 & mask) | g1; h2 = (h2 & mask) | g2; h3 = (h3 & mask) | g3; h4 = (h4 & mask) | g4;
    h0 = (h0 | (h1 << 26));
    h1 = ((h1 >> 6) | (h2 << 20));
    h2 = ((h2 >> 12) | (h3 << 14));
    h3 = ((h3 >> 18) | (h4 << 8));
    uint64_t f = (uint64_t)h0 + p->pad[0]; h0 = (uint32_t)f;
    f = (uint64_t)h1 + p->pad[1] + (f >> 32); h1 = (uint32_t)f;
    f = (uint64_t)h2 + p->pad[2] + (f >> 32); h2 = (uint32_t)f;
    f = (uint64_t)h3 + p->pad[3] + (f >> 32); h3 = (uint32_t)f;
    put_le32(tag, h0); put_le32(tag + 4, h1); put_le32(tag + 8, h2); put_le32(tag + 12, h3);
}

void poly1305(const uint8_t key[32], const uint8_t *msg, size_t len, uint8_t tag[16])
{
    struct poly p;
    poly_init(&p, key);
    poly_update(&p, msg, len);
    poly_final(&p, tag);
}

/* ------------------------------------------------------------------------
 * AEAD
 * ---------------------------------------------------------------------- */

static void aead_tag(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t alen, const uint8_t *ct,
                     size_t len, uint8_t tag[16])
{
    uint8_t block[64];
    chacha20_block(key, 0, nonce, block);
    struct poly p;
    poly_init(&p, block);
    static const uint8_t zeros[16];
    poly_update(&p, aad, alen);
    if (alen % 16) poly_update(&p, zeros, 16 - alen % 16);
    poly_update(&p, ct, len);
    if (len % 16) poly_update(&p, zeros, 16 - len % 16);
    uint8_t lens[16];
    for (int i = 0; i < 8; i++) {
        lens[i] = (uint8_t)((uint64_t)alen >> (8 * i));
        lens[8 + i] = (uint8_t)((uint64_t)len >> (8 * i));
    }
    poly_update(&p, lens, 16);
    poly_final(&p, tag);
}

void chachapoly_seal(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t alen, const uint8_t *in,
                     size_t len, uint8_t *out, uint8_t tag[16])
{
    chacha20_xor(key, 1, nonce, in, out, len);
    aead_tag(key, nonce, aad, alen, out, len, tag);
}

bool chachapoly_open(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t alen, const uint8_t *in,
                     size_t len, uint8_t *out, const uint8_t tag[16])
{
    uint8_t t[16];
    aead_tag(key, nonce, aad, alen, in, len, t);
    if (!ct_equal(t, tag, 16)) return false;
    chacha20_xor(key, 1, nonce, in, out, len);
    return true;
}

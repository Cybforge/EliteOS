/* AES-128/256 (FIPS 197) and GCM (NIST SP 800-38D) with 4-bit table GHASH. */
#include <crypto.h>

static uint8_t sbox[256];
static bool tables_ready;

static inline uint8_t rotl8(uint8_t x, int s) { return (uint8_t)((x << s) | (x >> (8 - s))); }

static void init_tables(void)
{
    /* the S-box from the multiplicative inverse in GF(2^8) plus the affine map */
    uint8_t p = 1, q = 1;
    do {
        p = (uint8_t)(p ^ (p << 1) ^ (p & 0x80 ? 0x1B : 0));
        q ^= (uint8_t)(q << 1);
        q ^= (uint8_t)(q << 2);
        q ^= (uint8_t)(q << 4);
        if (q & 0x80) q ^= 0x09;
        uint8_t x = (uint8_t)(q ^ rotl8(q, 1) ^ rotl8(q, 2) ^ rotl8(q, 3) ^ rotl8(q, 4));
        sbox[p] = x ^ 0x63;
    } while (p != 1);
    sbox[0] = 0x63;
    tables_ready = true;
}

static inline uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ (x & 0x80 ? 0x1B : 0)); }

static uint32_t sub_word(uint32_t w)
{
    return (uint32_t)sbox[w >> 24] << 24 | (uint32_t)sbox[(w >> 16) & 255] << 16 | (uint32_t)sbox[(w >> 8) & 255] << 8 |
           sbox[w & 255];
}

void aes_setkey(struct aes *a, const uint8_t *key, int keylen)
{
    if (!tables_ready) init_tables();
    int nk = keylen / 4;
    a->rounds = nk + 6;
    int total = 4 * (a->rounds + 1);
    for (int i = 0; i < nk; i++)
        a->rk[i] = (uint32_t)key[4 * i] << 24 | (uint32_t)key[4 * i + 1] << 16 | (uint32_t)key[4 * i + 2] << 8 | key[4 * i + 3];
    uint8_t rcon = 1;
    for (int i = nk; i < total; i++) {
        uint32_t t = a->rk[i - 1];
        if (i % nk == 0) {
            t = sub_word((t << 8) | (t >> 24)) ^ ((uint32_t)rcon << 24);
            rcon = xtime(rcon);
        } else if (nk > 6 && i % nk == 4) {
            t = sub_word(t);
        }
        a->rk[i] = a->rk[i - nk] ^ t;
    }
}

void aes_encrypt_block(const struct aes *a, const uint8_t in[16], uint8_t out[16])
{
    uint8_t s[16];
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) s[c * 4 + r] = in[c * 4 + r] ^ (uint8_t)(a->rk[c] >> (24 - 8 * r));
    for (int round = 1; round <= a->rounds; round++) {
        uint8_t t[16];
        /* SubBytes + ShiftRows */
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++) t[c * 4 + r] = sbox[s[((c + r) % 4) * 4 + r]];
        if (round != a->rounds) {
            /* MixColumns */
            for (int c = 0; c < 4; c++) {
                uint8_t *col = t + c * 4;
                uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                uint8_t all = a0 ^ a1 ^ a2 ^ a3;
                col[0] ^= all ^ xtime(a0 ^ a1);
                col[1] ^= all ^ xtime(a1 ^ a2);
                col[2] ^= all ^ xtime(a2 ^ a3);
                col[3] ^= all ^ xtime(a3 ^ a0);
            }
        }
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++) s[c * 4 + r] = t[c * 4 + r] ^ (uint8_t)(a->rk[round * 4 + c] >> (24 - 8 * r));
    }
    memcpy(out, s, 16);
}

/* ------------------------------------------------------------------------
 * GCM
 * ---------------------------------------------------------------------- */

static uint64_t be64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
    return v;
}

static void put_be64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (56 - 8 * i));
}

void gcm_setkey(struct gcm *g, const uint8_t *key, int keylen)
{
    aes_setkey(&g->aes, key, keylen);
    uint8_t h[16] = { 0 };
    aes_encrypt_block(&g->aes, h, h);
    uint64_t vh = be64(h), vl = be64(h + 8);
    g->tab_l[8] = vl;
    g->tab_h[8] = vh;
    g->tab_h[0] = g->tab_l[0] = 0;
    for (int i = 4; i > 0; i >>= 1) {
        uint32_t t = (uint32_t)(vl & 1) * 0xe1000000u;
        vl = (vh << 63) | (vl >> 1);
        vh = (vh >> 1) ^ ((uint64_t)t << 32);
        g->tab_l[i] = vl;
        g->tab_h[i] = vh;
    }
    for (int i = 2; i <= 8; i *= 2) {
        uint64_t *hl = g->tab_l + i, *hh = g->tab_h + i;
        vh = *hh;
        vl = *hl;
        for (int j = 1; j < i; j++) {
            hh[j] = vh ^ g->tab_h[j];
            hl[j] = vl ^ g->tab_l[j];
        }
    }
}

static const uint64_t last4[16] = { 0x0000, 0x1c20, 0x3840, 0x2460, 0x7080, 0x6ca0, 0x48c0, 0x54e0,
                                    0xe100, 0xfd20, 0xd940, 0xc560, 0x9180, 0x8da0, 0xa9c0, 0xb5e0 };

static void gmul(const struct gcm *g, uint8_t x[16])
{
    int lo = x[15] & 0xf;
    uint64_t zh = g->tab_h[lo], zl = g->tab_l[lo];
    for (int i = 15; i >= 0; i--) {
        lo = x[i] & 0xf;
        int hi = (x[i] >> 4) & 0xf;
        if (i != 15) {
            int rem = (int)(zl & 0xf);
            zl = (zh << 60) | (zl >> 4);
            zh = zh >> 4;
            zh ^= last4[rem] << 48;
            zh ^= g->tab_h[lo];
            zl ^= g->tab_l[lo];
        }
        int rem = (int)(zl & 0xf);
        zl = (zh << 60) | (zl >> 4);
        zh = zh >> 4;
        zh ^= last4[rem] << 48;
        zh ^= g->tab_h[hi];
        zl ^= g->tab_l[hi];
    }
    put_be64(x, zh);
    put_be64(x + 8, zl);
}

static void ghash_data(const struct gcm *g, uint8_t y[16], const uint8_t *d, size_t len)
{
    while (len) {
        size_t k = MIN(len, (size_t)16);
        for (size_t i = 0; i < k; i++) y[i] ^= d[i];
        gmul(g, y);
        d += k;
        len -= k;
    }
}

static void gcm_crypt(const struct gcm *g, const uint8_t iv[12], const uint8_t *in, size_t len, uint8_t *out)
{
    uint8_t ctr[16], ks[16];
    memcpy(ctr, iv, 12);
    uint32_t n = 2;                        /* counter 1 is reserved for the tag */
    for (size_t off = 0; off < len; off += 16, n++) {
        ctr[12] = (uint8_t)(n >> 24); ctr[13] = (uint8_t)(n >> 16); ctr[14] = (uint8_t)(n >> 8); ctr[15] = (uint8_t)n;
        aes_encrypt_block(&g->aes, ctr, ks);
        size_t k = MIN(len - off, (size_t)16);
        for (size_t i = 0; i < k; i++) out[off + i] = in[off + i] ^ ks[i];
    }
}

static void gcm_tag(const struct gcm *g, const uint8_t iv[12], const uint8_t *aad, size_t alen, const uint8_t *ct,
                    size_t len, uint8_t tag[16])
{
    uint8_t y[16] = { 0 }, lens[16], j0[16];
    ghash_data(g, y, aad, alen);
    ghash_data(g, y, ct, len);
    put_be64(lens, (uint64_t)alen * 8);
    put_be64(lens + 8, (uint64_t)len * 8);
    ghash_data(g, y, lens, 16);
    memcpy(j0, iv, 12);
    j0[12] = j0[13] = j0[14] = 0;
    j0[15] = 1;
    aes_encrypt_block(&g->aes, j0, j0);
    for (int i = 0; i < 16; i++) tag[i] = y[i] ^ j0[i];
}

void gcm_seal(const struct gcm *g, const uint8_t iv[12], const uint8_t *aad, size_t alen, const uint8_t *in, size_t len,
              uint8_t *out, uint8_t tag[16])
{
    gcm_crypt(g, iv, in, len, out);
    gcm_tag(g, iv, aad, alen, out, len, tag);
}

bool gcm_open(const struct gcm *g, const uint8_t iv[12], const uint8_t *aad, size_t alen, const uint8_t *in, size_t len,
              uint8_t *out, const uint8_t tag[16])
{
    uint8_t t[16];
    gcm_tag(g, iv, aad, alen, in, len, t);
    if (!ct_equal(t, tag, 16)) return false;
    gcm_crypt(g, iv, in, len, out);
    return true;
}

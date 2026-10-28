/* SHA-256, SHA-384, SHA-512 (FIPS 180-4), HMAC (RFC 2104), HKDF (RFC 5869). */
#include <crypto.h>

static inline uint32_t ror32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static inline uint64_t ror64(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

static const uint32_t k256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static void sha256_block(struct sha256 *c, const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4], f = c->h[5], g = c->h[6], h = c->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + k256[i] + w[i];
        uint32_t S0 = ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22);
        uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

void sha256_init(struct sha256 *c)
{
    static const uint32_t iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memcpy(c->h, iv, sizeof(iv));
    c->len = 0;
    c->n = 0;
}

void sha256_update(struct sha256 *c, const void *data, size_t len)
{
    const uint8_t *p = data;
    c->len += len;
    while (len) {
        if (!c->n && len >= 64) {
            sha256_block(c, p);
            p += 64;
            len -= 64;
            continue;
        }
        size_t k = MIN(len, 64 - c->n);
        memcpy(c->buf + c->n, p, k);
        c->n += k;
        p += k;
        len -= k;
        if (c->n == 64) { sha256_block(c, c->buf); c->n = 0; }
    }
}

void sha256_final(struct sha256 *c, uint8_t *out)
{
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80;
    sha256_update(c, &pad, 1);
    pad = 0;
    while (c->n != 56) sha256_update(c, &pad, 1);
    uint8_t lb[8];
    for (int i = 0; i < 8; i++) lb[i] = (uint8_t)(bits >> (56 - i * 8));
    sha256_update(c, lb, 8);
    for (int i = 0; i < 8; i++) {
        out[i * 4] = (uint8_t)(c->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(c->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)c->h[i];
    }
}

void sha256(const void *data, size_t len, uint8_t *out)
{
    struct sha256 c;
    sha256_init(&c);
    sha256_update(&c, data, len);
    sha256_final(&c, out);
}

/* ------------------------------------------------------------------------ */

static const uint64_t k512[80] = {
    0x428a2f98d728ae22ull, 0x7137449123ef65cdull, 0xb5c0fbcfec4d3b2full, 0xe9b5dba58189dbbcull, 0x3956c25bf348b538ull,
    0x59f111f1b605d019ull, 0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull, 0xd807aa98a3030242ull, 0x12835b0145706fbeull,
    0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull, 0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull, 0x9bdc06a725c71235ull,
    0xc19bf174cf692694ull, 0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull, 0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull,
    0x2de92c6f592b0275ull, 0x4a7484aa6ea6e483ull, 0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull, 0x983e5152ee66dfabull,
    0xa831c66d2db43210ull, 0xb00327c898fb213full, 0xbf597fc7beef0ee4ull, 0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull,
    0x06ca6351e003826full, 0x142929670a0e6e70ull, 0x27b70a8546d22ffcull, 0x2e1b21385c26c926ull, 0x4d2c6dfc5ac42aedull,
    0x53380d139d95b3dfull, 0x650a73548baf63deull, 0x766a0abb3c77b2a8ull, 0x81c2c92e47edaee6ull, 0x92722c851482353bull,
    0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull, 0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull, 0xd192e819d6ef5218ull,
    0xd69906245565a910ull, 0xf40e35855771202aull, 0x106aa07032bbd1b8ull, 0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull,
    0x2748774cdf8eeb99ull, 0x34b0bcb5e19b48a8ull, 0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull, 0x5b9cca4f7763e373ull,
    0x682e6ff3d6b2b8a3ull, 0x748f82ee5defb2fcull, 0x78a5636f43172f60ull, 0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull,
    0x90befffa23631e28ull, 0xa4506cebde82bde9ull, 0xbef9a3f7b2c67915ull, 0xc67178f2e372532bull, 0xca273eceea26619cull,
    0xd186b8c721c0c207ull, 0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull, 0x06f067aa72176fbaull, 0x0a637dc5a2c898a6ull,
    0x113f9804bef90daeull, 0x1b710b35131c471bull, 0x28db77f523047d84ull, 0x32caab7b40c72493ull, 0x3c9ebe0a15c9bebcull,
    0x431d67c49c100d4cull, 0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull, 0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull,
};

static void sha512_block(struct sha512 *c, const uint8_t *p)
{
    uint64_t w[80];
    for (int i = 0; i < 16; i++) {
        uint64_t v = 0;
        for (int k = 0; k < 8; k++) v = (v << 8) | p[i * 8 + k];
        w[i] = v;
    }
    for (int i = 16; i < 80; i++) {
        uint64_t s0 = ror64(w[i - 15], 1) ^ ror64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        uint64_t s1 = ror64(w[i - 2], 19) ^ ror64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint64_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4], f = c->h[5], g = c->h[6], h = c->h[7];
    for (int i = 0; i < 80; i++) {
        uint64_t S1 = ror64(e, 14) ^ ror64(e, 18) ^ ror64(e, 41);
        uint64_t ch = (e & f) ^ (~e & g);
        uint64_t t1 = h + S1 + ch + k512[i] + w[i];
        uint64_t S0 = ror64(a, 28) ^ ror64(a, 34) ^ ror64(a, 39);
        uint64_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        uint64_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

void sha512_init(struct sha512 *c)
{
    static const uint64_t iv[8] = { 0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull, 0x3c6ef372fe94f82bull, 0xa54ff53a5f1d36f1ull,
                                    0x510e527fade682d1ull, 0x9b05688c2b3e6c1full, 0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull };
    memcpy(c->h, iv, sizeof(iv));
    c->len = 0;
    c->n = 0;
    c->out = 64;
}

void sha384_init(struct sha512 *c)
{
    static const uint64_t iv[8] = { 0xcbbb9d5dc1059ed8ull, 0x629a292a367cd507ull, 0x9159015a3070dd17ull, 0x152fecd8f70e5939ull,
                                    0x67332667ffc00b31ull, 0x8eb44a8768581511ull, 0xdb0c2e0d64f98fa7ull, 0x47b5481dbefa4fa4ull };
    memcpy(c->h, iv, sizeof(iv));
    c->len = 0;
    c->n = 0;
    c->out = 48;
}

void sha512_update(struct sha512 *c, const void *data, size_t len)
{
    const uint8_t *p = data;
    c->len += len;
    while (len) {
        if (!c->n && len >= 128) {
            sha512_block(c, p);
            p += 128;
            len -= 128;
            continue;
        }
        size_t k = MIN(len, 128 - c->n);
        memcpy(c->buf + c->n, p, k);
        c->n += k;
        p += k;
        len -= k;
        if (c->n == 128) { sha512_block(c, c->buf); c->n = 0; }
    }
}

void sha512_final(struct sha512 *c, uint8_t *out)
{
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80;
    sha512_update(c, &pad, 1);
    pad = 0;
    while (c->n != 112) sha512_update(c, &pad, 1);
    uint8_t lb[16] = { 0 };
    for (int i = 0; i < 8; i++) lb[8 + i] = (uint8_t)(bits >> (56 - i * 8));
    sha512_update(c, lb, 16);
    for (int i = 0; i < c->out / 8; i++)
        for (int k = 0; k < 8; k++) out[i * 8 + k] = (uint8_t)(c->h[i] >> (56 - k * 8));
}

void sha384(const void *data, size_t len, uint8_t *out)
{
    struct sha512 c;
    sha384_init(&c);
    sha512_update(&c, data, len);
    sha512_final(&c, out);
}

void sha512(const void *data, size_t len, uint8_t *out)
{
    struct sha512 c;
    sha512_init(&c);
    sha512_update(&c, data, len);
    sha512_final(&c, out);
}

/* ------------------------------------------------------------------------
 * generic hash, HMAC, HKDF
 * ---------------------------------------------------------------------- */

int hash_len(int alg) { return alg == HASH_SHA256 ? 32 : alg == HASH_SHA384 ? 48 : 64; }
int hash_block(int alg) { return alg == HASH_SHA256 ? 64 : 128; }

void hash_init(struct hash_ctx *c, int alg)
{
    c->alg = alg;
    if (alg == HASH_SHA256) sha256_init(&c->u.s256);
    else if (alg == HASH_SHA384) sha384_init(&c->u.s512);
    else sha512_init(&c->u.s512);
}

void hash_update(struct hash_ctx *c, const void *data, size_t len)
{
    if (c->alg == HASH_SHA256) sha256_update(&c->u.s256, data, len);
    else sha512_update(&c->u.s512, data, len);
}

void hash_final(struct hash_ctx *c, uint8_t *out)
{
    if (c->alg == HASH_SHA256) sha256_final(&c->u.s256, out);
    else sha512_final(&c->u.s512, out);
}

void hash_once(int alg, const void *data, size_t len, uint8_t *out)
{
    struct hash_ctx c;
    hash_init(&c, alg);
    hash_update(&c, data, len);
    hash_final(&c, out);
}

void hmac(int alg, const uint8_t *key, size_t klen, const void *data, size_t len, uint8_t *out)
{
    int bs = hash_block(alg), hl = hash_len(alg);
    uint8_t k[HASH_BLOCK_MAX] = { 0 }, pad[HASH_BLOCK_MAX], inner[HASH_MAX];
    if ((int)klen > bs) hash_once(alg, key, klen, k);
    else memcpy(k, key, klen);
    struct hash_ctx c;
    for (int i = 0; i < bs; i++) pad[i] = k[i] ^ 0x36;
    hash_init(&c, alg);
    hash_update(&c, pad, (size_t)bs);
    hash_update(&c, data, len);
    hash_final(&c, inner);
    for (int i = 0; i < bs; i++) pad[i] = k[i] ^ 0x5c;
    hash_init(&c, alg);
    hash_update(&c, pad, (size_t)bs);
    hash_update(&c, inner, (size_t)hl);
    hash_final(&c, out);
}

void hkdf_extract(int alg, const uint8_t *salt, size_t slen, const uint8_t *ikm, size_t ilen, uint8_t *prk)
{
    uint8_t zero[HASH_MAX] = { 0 };
    if (!salt || !slen) { salt = zero; slen = (size_t)hash_len(alg); }
    hmac(alg, salt, slen, ikm, ilen, prk);
}

void hkdf_expand(int alg, const uint8_t *prk, const uint8_t *info, size_t ilen, uint8_t *out, size_t olen)
{
    int hl = hash_len(alg);
    uint8_t t[HASH_MAX], buf[HASH_MAX + 512 + 1];
    size_t tlen = 0, done = 0;
    for (uint8_t i = 1; done < olen; i++) {
        size_t n = 0;
        memcpy(buf, t, tlen);
        n += tlen;
        if (ilen > 512) ilen = 512;
        memcpy(buf + n, info, ilen);
        n += ilen;
        buf[n++] = i;
        hmac(alg, prk, (size_t)hl, buf, n, t);
        tlen = (size_t)hl;
        size_t k = MIN((size_t)hl, olen - done);
        memcpy(out + done, t, k);
        done += k;
    }
}

bool ct_equal(const void *a, const void *b, size_t len)
{
    const uint8_t *x = a, *y = b;
    uint8_t d = 0;
    for (size_t i = 0; i < len; i++) d |= x[i] ^ y[i];
    return d == 0;
}

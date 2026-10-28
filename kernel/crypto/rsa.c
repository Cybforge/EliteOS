/* RSA signature verification: PKCS #1 v1.5 and PSS (RFC 8017). */
#include <crypto.h>
#include <mm.h>
#include "bignum.h"

/* sig^e mod n into em (k bytes, k = length of n) */
static bool rsa_public(const uint8_t *n, size_t nlen, const uint8_t *e, size_t elen, const uint8_t *sig, size_t slen,
                       uint8_t *em, size_t *klen)
{
    while (nlen && !*n) { n++; nlen--; }
    if (nlen < 64 || nlen > BN_LIMBS * 4 || slen > nlen + 1 || elen > 8) return false;
    struct mont *M = kmalloc(sizeof(*M));
    bn_t *t = kmalloc(sizeof(bn_t) * 4);
    if (!M || !t) { kfree(M); kfree(t); return false; }
    bn_t *nb = &t[0], *s = &t[1], *sm = &t[2], *ex = &t[3];
    bn_from_bytes(nb, n, nlen);
    int limbs = (int)((nlen + 3) / 4);
    bool ok = mont_init(M, nb, limbs);
    if (ok) {
        bn_from_bytes(s, sig, slen);
        ok = bn_cmp(s, nb, limbs) < 0;
    }
    if (ok) {
        bn_from_bytes(ex, e, elen);
        mont_to(M, sm, s);
        mont_pow(M, sm, sm, ex, bn_bits(ex, 2));
        mont_from(M, s, sm);
        bn_to_bytes(s, limbs, em, nlen);
        *klen = nlen;
    }
    kfree(M);
    kfree(t);
    return ok;
}

static const uint8_t di_sha256[] = { 0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20 };
static const uint8_t di_sha384[] = { 0x30, 0x41, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x02, 0x05, 0x00, 0x04, 0x30 };
static const uint8_t di_sha512[] = { 0x30, 0x51, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x03, 0x05, 0x00, 0x04, 0x40 };

bool rsa_verify_pkcs1(const uint8_t *n, size_t nlen, const uint8_t *e, size_t elen, int hash_alg, const uint8_t *hash,
                      const uint8_t *sig, size_t slen)
{
    uint8_t *em = kmalloc(BN_LIMBS * 4);
    size_t k = 0;
    bool ok = em && rsa_public(n, nlen, e, elen, sig, slen, em, &k);
    if (ok) {
        const uint8_t *di = hash_alg == HASH_SHA256 ? di_sha256 : hash_alg == HASH_SHA384 ? di_sha384 : di_sha512;
        size_t dl = sizeof(di_sha256), hl = (size_t)hash_len(hash_alg);
        size_t tl = dl + hl;
        ok = k >= tl + 11 && em[0] == 0 && em[1] == 1;
        for (size_t i = 2; ok && i < k - tl - 1; i++) ok = em[i] == 0xFF;
        ok = ok && em[k - tl - 1] == 0 && !memcmp(em + k - tl, di, dl) && ct_equal(em + k - hl, hash, hl);
    }
    kfree(em);
    return ok;
}

static void mgf1(int alg, const uint8_t *seed, size_t slen, uint8_t *mask, size_t len)
{
    uint8_t buf[HASH_MAX + 4], out[HASH_MAX];
    size_t hl = (size_t)hash_len(alg), done = 0;
    memcpy(buf, seed, slen);
    for (uint32_t c = 0; done < len; c++) {
        buf[slen] = (uint8_t)(c >> 24); buf[slen + 1] = (uint8_t)(c >> 16);
        buf[slen + 2] = (uint8_t)(c >> 8); buf[slen + 3] = (uint8_t)c;
        hash_once(alg, buf, slen + 4, out);
        size_t k = MIN(hl, len - done);
        for (size_t i = 0; i < k; i++) mask[done + i] ^= out[i];
        done += k;
    }
}

bool rsa_verify_pss(const uint8_t *n, size_t nlen, const uint8_t *e, size_t elen, int hash_alg, const uint8_t *hash,
                    const uint8_t *sig, size_t slen)
{
    uint8_t *em = kmalloc(BN_LIMBS * 4);
    size_t k = 0;
    bool ok = em && rsa_public(n, nlen, e, elen, sig, slen, em, &k);
    if (ok) {
        while (nlen && !*n) { n++; nlen--; }
        int top = n[0];
        int modbits = (int)(nlen - 1) * 8;
        while (top) { modbits++; top >>= 1; }
        int embits = modbits - 1;
        size_t emlen = (size_t)(embits + 7) / 8;
        const uint8_t *EM = em + (k - emlen);
        if (k > emlen && em[0]) ok = false;
        size_t hl = (size_t)hash_len(hash_alg);
        if (ok && (emlen < hl + 2 || EM[emlen - 1] != 0xbc)) ok = false;
        if (ok) {
            size_t dblen = emlen - hl - 1;
            uint8_t *db = kmalloc(dblen);
            memcpy(db, EM, dblen);
            const uint8_t *H = EM + dblen;
            int zbits = 8 * (int)emlen - embits;
            if (EM[0] & (uint8_t)(0xFF << (8 - zbits))) ok = false;
            mgf1(hash_alg, H, hl, db, dblen);
            db[0] &= (uint8_t)(0xFF >> zbits);
            size_t i = 0;
            while (i < dblen && db[i] == 0) i++;
            if (ok && (i >= dblen || db[i] != 1)) ok = false;
            if (ok) {
                size_t saltlen = dblen - i - 1;
                uint8_t *mp = kmalloc(8 + hl + saltlen), hh[HASH_MAX];
                memset(mp, 0, 8);
                memcpy(mp + 8, hash, hl);
                memcpy(mp + 8 + hl, db + i + 1, saltlen);
                hash_once(hash_alg, mp, 8 + hl + saltlen, hh);
                ok = ct_equal(hh, H, hl);
                kfree(mp);
            }
            kfree(db);
        }
    }
    kfree(em);
    return ok;
}

/* X25519 (RFC 7748), RSA PKCS#1/PSS, ECDSA and ECDH P-256/P-384 against keys and signatures made by openssl. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "crypto.h"
#include "vectors.h"
static int fails;
void *kmalloc(size_t n) { return malloc(n); }
void *kzalloc(size_t n) { return calloc(1, n); }
void kfree(void *p) { free(p); }
void random_bytes(void *o, size_t n) { for (size_t i = 0; i < n; i++) ((uint8_t *)o)[i] = (uint8_t)rand(); }
static size_t hex(const char *h, uint8_t *o) { size_t l = strlen(h) / 2; for (size_t i = 0; i < l; i++) sscanf(h + 2 * i, "%2hhx", &o[i]); return l; }
static void res(const char *name, int ok) { printf("%s %s\n", ok ? "ok  " : "FAIL", name); if (!ok) fails++; }
int main(void) {
    uint8_t a[1024], b[1024], c[1024], d[1024], out[64];
    /* X25519 RFC 7748 section 5.2 and 6.1 */
    hex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", a);
    hex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", b);
    x25519(out, a, b); hex("c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552", c); res("x25519 rfc7748 5.2", !memcmp(out, c, 32));
    hex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", a);
    x25519_base(out, a); hex("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a", c); res("x25519 alice public", !memcmp(out, c, 32));
    hex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb", b);
    uint8_t bob_pub[32], s1[32], s2[32]; x25519_base(bob_pub, b);
    hex("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f", d); res("x25519 bob public", !memcmp(bob_pub, d, 32));
    x25519(s1, a, bob_pub); uint8_t alice_pub[32]; x25519_base(alice_pub, a); x25519(s2, b, alice_pub);
    hex("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742", c); res("x25519 shared secret", !memcmp(s1, c, 32) && !memcmp(s2, c, 32));
    /* RSA */
    uint8_t e[3] = { 1, 0, 1 }, hsh[64];
    for (unsigned i = 0; i < sizeof(rsa_vec) / sizeof(rsa_vec[0]); i++) {
        size_t nl = hex(rsa_vec[i].n, a), s1l = hex(rsa_vec[i].pkcs1, b), s2l = hex(rsa_vec[i].pss, c);
        int alg = rsa_vec[i].hash == 256 ? HASH_SHA256 : HASH_SHA384;
        hash_once(alg, msg, strlen(msg), hsh);
        char name[64];
        snprintf(name, sizeof(name), "rsa-%d sha%d pkcs1", rsa_vec[i].bits, rsa_vec[i].hash); res(name, rsa_verify_pkcs1(a, nl, e, 3, alg, hsh, b, s1l));
        snprintf(name, sizeof(name), "rsa-%d sha%d pss", rsa_vec[i].bits, rsa_vec[i].hash); res(name, rsa_verify_pss(a, nl, e, 3, alg, hsh, c, s2l));
        hsh[5] ^= 1;
        snprintf(name, sizeof(name), "rsa-%d rejects wrong hash", rsa_vec[i].bits);
        res(name, !rsa_verify_pkcs1(a, nl, e, 3, alg, hsh, b, s1l) && !rsa_verify_pss(a, nl, e, 3, alg, hsh, c, s2l));
    }
    /* ECDSA */
    for (unsigned i = 0; i < sizeof(ec_vec) / sizeof(ec_vec[0]); i++) {
        size_t pl = hex(ec_vec[i].pub, a), rl = hex(ec_vec[i].r, b), sl = hex(ec_vec[i].s, c);
        int alg = ec_vec[i].hash == 256 ? HASH_SHA256 : HASH_SHA384;
        hash_once(alg, msg, strlen(msg), hsh);
        char name[64];
        snprintf(name, sizeof(name), "ecdsa %s sha%d", ec_vec[i].curve ? "P-384" : "P-256", ec_vec[i].hash);
        res(name, ecdsa_verify(ec_vec[i].curve, a, pl, hsh, (size_t)hash_len(alg), b, rl, c, sl));
        c[3] ^= 0x10;
        snprintf(name, sizeof(name), "ecdsa %s rejects bad signature", ec_vec[i].curve ? "P-384" : "P-256");
        res(name, !ecdsa_verify(ec_vec[i].curve, a, pl, hsh, (size_t)hash_len(alg), b, rl, c, sl));
    }
    /* ECDH */
    for (unsigned i = 0; i < sizeof(ecdh_vec) / sizeof(ecdh_vec[0]); i++) {
        hex(ecdh_vec[i].priv, a); size_t pl = hex(ecdh_vec[i].peer, b); size_t sl = hex(ecdh_vec[i].shared, c);
        res(ecdh_vec[i].curve ? "ecdh P-384" : "ecdh P-256", ec_ecdh(ecdh_vec[i].curve, a, b, pl, d) && !memcmp(d, c, sl));
        b[10] ^= 1;
        res("ecdh rejects a point off the curve", !ec_ecdh(ecdh_vec[i].curve, a, b, pl, d));
    }
    /* keygen round trip: A = a*G, B = b*G, a*B == b*A */
    uint8_t pa[48], pb[48], qa[97], qb[97], sa[48], sb[48];
    for (int cv = 0; cv < 2; cv++) {
        int L = ec_bytes(cv);
        ec_keygen(cv, pa, qa); ec_keygen(cv, pb, qb);
        res(cv ? "ec keygen+ecdh P-384" : "ec keygen+ecdh P-256",
            ec_ecdh(cv, pa, qb, 2 * L + 1, sa) && ec_ecdh(cv, pb, qa, 2 * L + 1, sb) && !memcmp(sa, sb, L));
    }
    printf("\n%d failures\n", fails);
    return fails != 0;
}

#ifndef ZENITH_CRYPTO_H
#define ZENITH_CRYPTO_H

/*
 * Cryptography for TLS: hashes, MACs, key derivation, AEAD ciphers,
 * key exchange (X25519, P-256), signature checks (RSA, ECDSA P-256/P-384)
 * and a random number generator. All written for ZenithOS.
 */
#include <kernel.h>

/* ---- hashes ---- */
#define SHA256_LEN 32
#define SHA384_LEN 48
#define SHA512_LEN 64
#define HASH_MAX   64
#define HASH_BLOCK_MAX 128

struct sha256 { uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t n; };
struct sha512 { uint64_t h[8]; uint64_t len; uint8_t buf[128]; size_t n; int out; };

void sha256_init(struct sha256 *c);
void sha256_update(struct sha256 *c, const void *data, size_t len);
void sha256_final(struct sha256 *c, uint8_t *out);
void sha256(const void *data, size_t len, uint8_t *out);
void sha384_init(struct sha512 *c);
void sha512_init(struct sha512 *c);
void sha512_update(struct sha512 *c, const void *data, size_t len);
void sha512_final(struct sha512 *c, uint8_t *out);      /* 48 bytes for SHA-384 */
void sha384(const void *data, size_t len, uint8_t *out);
void sha512(const void *data, size_t len, uint8_t *out);

/* SHA-1 is only used to recognise old certificate signatures (never trusted) */

enum { HASH_SHA256, HASH_SHA384, HASH_SHA512 };

struct hash_ctx {
    int alg;
    union { struct sha256 s256; struct sha512 s512; } u;
};
int  hash_len(int alg);
int  hash_block(int alg);
void hash_init(struct hash_ctx *c, int alg);
void hash_update(struct hash_ctx *c, const void *data, size_t len);
void hash_final(struct hash_ctx *c, uint8_t *out);
void hash_once(int alg, const void *data, size_t len, uint8_t *out);

void hmac(int alg, const uint8_t *key, size_t klen, const void *data, size_t len, uint8_t *out);
/* password hashing: PBKDF2-HMAC-SHA256 */
void pbkdf2_sha256(const uint8_t *pass, size_t plen, const uint8_t *salt, size_t slen, uint32_t iterations,
                   uint8_t *out, size_t outlen);

void hkdf_extract(int alg, const uint8_t *salt, size_t slen, const uint8_t *ikm, size_t ilen, uint8_t *prk);
void hkdf_expand(int alg, const uint8_t *prk, const uint8_t *info, size_t ilen, uint8_t *out, size_t olen);

/* ---- AES and GCM ---- */
struct aes { uint32_t rk[60]; int rounds; };
void aes_setkey(struct aes *a, const uint8_t *key, int keylen);   /* 16 or 32 */
void aes_encrypt_block(const struct aes *a, const uint8_t in[16], uint8_t out[16]);

struct gcm { struct aes aes; uint64_t hh, hl; uint64_t tab_h[16], tab_l[16]; };
void gcm_setkey(struct gcm *g, const uint8_t *key, int keylen);
void gcm_seal(const struct gcm *g, const uint8_t iv[12], const uint8_t *aad, size_t alen,
              const uint8_t *in, size_t len, uint8_t *out, uint8_t tag[16]);
bool gcm_open(const struct gcm *g, const uint8_t iv[12], const uint8_t *aad, size_t alen,
              const uint8_t *in, size_t len, uint8_t *out, const uint8_t tag[16]);

/* ---- ChaCha20-Poly1305 (RFC 8439) ---- */
void chacha20_block(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], uint8_t out[64]);
void chacha20_xor(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], const uint8_t *in, uint8_t *out, size_t len);
void poly1305(const uint8_t key[32], const uint8_t *msg, size_t len, uint8_t tag[16]);
void chachapoly_seal(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t alen,
                     const uint8_t *in, size_t len, uint8_t *out, uint8_t tag[16]);
bool chachapoly_open(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t alen,
                     const uint8_t *in, size_t len, uint8_t *out, const uint8_t tag[16]);

/* ---- key exchange ---- */
void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);
void x25519_base(uint8_t out[32], const uint8_t scalar[32]);

enum { CURVE_P256, CURVE_P384 };
int  ec_bytes(int curve);                                               /* 32 or 48 */
bool ec_keygen(int curve, uint8_t *priv, uint8_t *pub);                 /* pub: 04 || X || Y */
bool ec_ecdh(int curve, const uint8_t *priv, const uint8_t *peer_pub, size_t plen, uint8_t *shared);
bool ecdsa_verify(int curve, const uint8_t *pub, size_t plen, const uint8_t *hash, size_t hlen,
                  const uint8_t *r, size_t rlen, const uint8_t *s, size_t slen);

/* ---- RSA signature checks ---- */
bool rsa_verify_pkcs1(const uint8_t *n, size_t nlen, const uint8_t *e, size_t elen, int hash_alg,
                      const uint8_t *hash, const uint8_t *sig, size_t slen);
bool rsa_verify_pss(const uint8_t *n, size_t nlen, const uint8_t *e, size_t elen, int hash_alg,
                    const uint8_t *hash, const uint8_t *sig, size_t slen);

/* ---- randomness ---- */
void random_bytes(void *out, size_t len);

/* compares in constant time; true if equal */
bool ct_equal(const void *a, const void *b, size_t len);

#endif

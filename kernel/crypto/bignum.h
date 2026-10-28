#ifndef ZENITH_BIGNUM_H
#define ZENITH_BIGNUM_H

/*
 * Fixed-width multi-precision integers for RSA and elliptic curves:
 * little-endian 32-bit limbs, arithmetic modulo an odd number in
 * Montgomery form.
 */
#include <kernel.h>

#define BN_LIMBS 136            /* up to 4352 bits: RSA-4096 roots */

typedef struct { uint32_t d[BN_LIMBS]; } bn_t;

struct mont {
    int n;                      /* limbs in use */
    bn_t m;                     /* modulus (odd) */
    uint32_t minv;              /* -m^-1 mod 2^32 */
    bn_t rr;                    /* R^2 mod m */
    bn_t one;                   /* R mod m (1 in Montgomery form) */
};

void bn_zero(bn_t *a);
void bn_from_bytes(bn_t *a, const uint8_t *be, size_t len);
void bn_to_bytes(const bn_t *a, int n, uint8_t *be, size_t len);
int  bn_cmp(const bn_t *a, const bn_t *b, int n);
bool bn_is_zero(const bn_t *a, int n);
int  bn_bits(const bn_t *a, int n);
bool bn_bit(const bn_t *a, int i);

bool mont_init(struct mont *c, const bn_t *m, int n);
void mont_mul(const struct mont *c, bn_t *r, const bn_t *a, const bn_t *b);
void mont_to(const struct mont *c, bn_t *r, const bn_t *a);       /* a must be < m */
void mont_from(const struct mont *c, bn_t *r, const bn_t *a);
void mod_add(const struct mont *c, bn_t *r, const bn_t *a, const bn_t *b);
void mod_sub(const struct mont *c, bn_t *r, const bn_t *a, const bn_t *b);
void mod_reduce(const struct mont *c, bn_t *a);                   /* a < 2m -> a mod m */
void mont_pow(const struct mont *c, bn_t *r, const bn_t *base_m, const bn_t *e, int ebits);   /* Montgomery in/out */
void mont_inv(const struct mont *c, bn_t *r, const bn_t *a_m);    /* prime modulus, Fermat */

#endif

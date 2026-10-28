/*
 * NIST P-256 and P-384: ECDH and ECDSA verification. Jacobian coordinates
 * in the Montgomery domain, a = -3 doubling formulas.
 */
#include <crypto.h>
#include <spinlock.h>
#include "bignum.h"

struct curve {
    int bytes, limbs;
    struct mont p, n;
    bn_t b_m, gx_m, gy_m;
    bool ready;
};

typedef struct { bn_t x, y, z; } pt_t;

static struct curve curves[2];
static spinlock_t init_lock = SPINLOCK_INIT("ec");

static const char *const params[2][5] = {
    { "ffffffff00000001000000000000000000000000ffffffffffffffffffffffff",
      "5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b",
      "ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551",
      "6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296",
      "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5" },
    { "fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffeffffffff0000000000000000ffffffff",
      "b3312fa7e23ee7e4988e056be3f82d19181d9c6efe8141120314088f5013875ac656398d8a2ed19d2a85c8edd3ec2aef",
      "ffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf581a0db248b0a77aecec196accc52973",
      "aa87ca22be8b05378eb1c71ef320ad746e1d3b628ba79b9859f741e082542a385502f25dbf55296c3a545e3872760ab7",
      "3617de4a96262c6f5d9e98bf9292dc29f8f41dbd289a147ce9da3113b5f0b8c00a60b1ce1d7e819d7a431d7c90ea0e5f" },
};

static void from_hex(bn_t *a, const char *h)
{
    uint8_t buf[64];
    size_t n = strlen(h) / 2;
    for (size_t i = 0; i < n; i++) {
        int hi = h[2 * i], lo = h[2 * i + 1];
        hi = hi <= '9' ? hi - '0' : hi - 'a' + 10;
        lo = lo <= '9' ? lo - '0' : lo - 'a' + 10;
        buf[i] = (uint8_t)(hi << 4 | lo);
    }
    bn_from_bytes(a, buf, n);
}

static struct curve *get_curve(int id)
{
    if (id != CURVE_P256 && id != CURVE_P384) return NULL;
    struct curve *c = &curves[id];
    spin_lock(&init_lock);
    if (!c->ready) {
        c->bytes = id == CURVE_P256 ? 32 : 48;
        c->limbs = c->bytes / 4;
        bn_t p, b, n, gx, gy;
        from_hex(&p, params[id][0]);
        from_hex(&b, params[id][1]);
        from_hex(&n, params[id][2]);
        from_hex(&gx, params[id][3]);
        from_hex(&gy, params[id][4]);
        mont_init(&c->p, &p, c->limbs);
        mont_init(&c->n, &n, c->limbs);
        mont_to(&c->p, &c->b_m, &b);
        mont_to(&c->p, &c->gx_m, &gx);
        mont_to(&c->p, &c->gy_m, &gy);
        c->ready = true;
    }
    spin_unlock(&init_lock);
    return c;
}

int ec_bytes(int curve) { return curve == CURVE_P384 ? 48 : 32; }

static void pt_dbl(const struct curve *C, pt_t *r, const pt_t *p)
{
    const struct mont *F = &C->p;
    if (bn_is_zero(&p->z, C->limbs)) { *r = *p; return; }
    bn_t delta, gamma, beta, alpha, t1, t2, t3, beta4, x3, y3, z3;
    mont_mul(F, &delta, &p->z, &p->z);
    mont_mul(F, &gamma, &p->y, &p->y);
    mont_mul(F, &beta, &p->x, &gamma);
    mod_sub(F, &t1, &p->x, &delta);
    mod_add(F, &t2, &p->x, &delta);
    mont_mul(F, &t3, &t1, &t2);
    mod_add(F, &alpha, &t3, &t3);
    mod_add(F, &alpha, &alpha, &t3);
    mod_add(F, &beta4, &beta, &beta);
    mod_add(F, &beta4, &beta4, &beta4);
    mont_mul(F, &x3, &alpha, &alpha);
    mod_sub(F, &x3, &x3, &beta4);
    mod_sub(F, &x3, &x3, &beta4);
    mod_add(F, &t1, &p->y, &p->z);
    mont_mul(F, &z3, &t1, &t1);
    mod_sub(F, &z3, &z3, &gamma);
    mod_sub(F, &z3, &z3, &delta);
    mod_sub(F, &t1, &beta4, &x3);
    mont_mul(F, &t2, &alpha, &t1);
    mont_mul(F, &t3, &gamma, &gamma);
    mod_add(F, &t3, &t3, &t3);
    mod_add(F, &t3, &t3, &t3);
    mod_add(F, &t3, &t3, &t3);
    mod_sub(F, &y3, &t2, &t3);
    r->x = x3;
    r->y = y3;
    r->z = z3;
}

static void pt_add(const struct curve *C, pt_t *r, const pt_t *p, const pt_t *q)
{
    const struct mont *F = &C->p;
    int n = C->limbs;
    if (bn_is_zero(&p->z, n)) { *r = *q; return; }
    if (bn_is_zero(&q->z, n)) { *r = *p; return; }
    bn_t z1z1, z2z2, u1, u2, s1, s2, h, i, j, rr, v, t, x3, y3, z3;
    mont_mul(F, &z1z1, &p->z, &p->z);
    mont_mul(F, &z2z2, &q->z, &q->z);
    mont_mul(F, &u1, &p->x, &z2z2);
    mont_mul(F, &u2, &q->x, &z1z1);
    mont_mul(F, &t, &q->z, &z2z2);
    mont_mul(F, &s1, &p->y, &t);
    mont_mul(F, &t, &p->z, &z1z1);
    mont_mul(F, &s2, &q->y, &t);
    mod_sub(F, &h, &u2, &u1);
    mod_sub(F, &rr, &s2, &s1);
    if (bn_is_zero(&h, n)) {
        if (bn_is_zero(&rr, n)) { pt_dbl(C, r, p); return; }
        bn_zero(&r->z);                          /* P + (-P) = infinity */
        return;
    }
    mod_add(F, &t, &h, &h);
    mont_mul(F, &i, &t, &t);
    mont_mul(F, &j, &h, &i);
    mod_add(F, &rr, &rr, &rr);
    mont_mul(F, &v, &u1, &i);
    mont_mul(F, &x3, &rr, &rr);
    mod_sub(F, &x3, &x3, &j);
    mod_sub(F, &x3, &x3, &v);
    mod_sub(F, &x3, &x3, &v);
    mod_sub(F, &t, &v, &x3);
    mont_mul(F, &y3, &rr, &t);
    mont_mul(F, &t, &s1, &j);
    mod_sub(F, &y3, &y3, &t);
    mod_sub(F, &y3, &y3, &t);
    mod_add(F, &t, &p->z, &q->z);
    mont_mul(F, &z3, &t, &t);
    mod_sub(F, &z3, &z3, &z1z1);
    mod_sub(F, &z3, &z3, &z2z2);
    mont_mul(F, &z3, &z3, &h);
    r->x = x3;
    r->y = y3;
    r->z = z3;
}

static void pt_mul(const struct curve *C, pt_t *r, const pt_t *p, const bn_t *k)
{
    pt_t acc;
    bn_zero(&acc.x);
    bn_zero(&acc.y);
    bn_zero(&acc.z);
    for (int i = bn_bits(k, C->limbs) - 1; i >= 0; i--) {
        pt_dbl(C, &acc, &acc);
        if (bn_bit(k, i)) pt_add(C, &acc, &acc, p);
    }
    *r = acc;
}

/* affine x, y (normal domain) */
static bool pt_affine(const struct curve *C, const pt_t *p, bn_t *x, bn_t *y)
{
    const struct mont *F = &C->p;
    if (bn_is_zero(&p->z, C->limbs)) return false;
    bn_t zi, zi2, t;
    mont_inv(F, &zi, &p->z);
    mont_mul(F, &zi2, &zi, &zi);
    mont_mul(F, &t, &p->x, &zi2);
    mont_from(F, x, &t);
    if (y) {
        mont_mul(F, &t, &zi2, &zi);
        mont_mul(F, &t, &p->y, &t);
        mont_from(F, y, &t);
    }
    return true;
}

/* parse 04 || X || Y and check that the point is on the curve */
static bool pt_parse(const struct curve *C, const uint8_t *pub, size_t len, pt_t *out)
{
    if (len != (size_t)(1 + 2 * C->bytes) || pub[0] != 4) return false;
    const struct mont *F = &C->p;
    bn_t x, y;
    bn_from_bytes(&x, pub + 1, (size_t)C->bytes);
    bn_from_bytes(&y, pub + 1 + C->bytes, (size_t)C->bytes);
    if (bn_cmp(&x, &F->m, C->limbs) >= 0 || bn_cmp(&y, &F->m, C->limbs) >= 0) return false;
    mont_to(F, &out->x, &x);
    mont_to(F, &out->y, &y);
    out->z = F->one;
    /* y^2 == x^3 - 3x + b */
    bn_t y2, x3, t;
    mont_mul(F, &y2, &out->y, &out->y);
    mont_mul(F, &t, &out->x, &out->x);
    mont_mul(F, &x3, &t, &out->x);
    mod_sub(F, &x3, &x3, &out->x);
    mod_sub(F, &x3, &x3, &out->x);
    mod_sub(F, &x3, &x3, &out->x);
    mod_add(F, &x3, &x3, &C->b_m);
    return bn_cmp(&y2, &x3, C->limbs) == 0;
}

static void generator(const struct curve *C, pt_t *g)
{
    g->x = C->gx_m;
    g->y = C->gy_m;
    g->z = C->p.one;
}

bool ec_keygen(int curve, uint8_t *priv, uint8_t *pub)
{
    struct curve *C = get_curve(curve);
    if (!C) return false;
    bn_t d;
    for (int tries = 0; tries < 100; tries++) {
        random_bytes(priv, (size_t)C->bytes);
        bn_from_bytes(&d, priv, (size_t)C->bytes);
        if (!bn_is_zero(&d, C->limbs) && bn_cmp(&d, &C->n.m, C->limbs) < 0) break;
    }
    pt_t g, q;
    generator(C, &g);
    pt_mul(C, &q, &g, &d);
    bn_t x, y;
    if (!pt_affine(C, &q, &x, &y)) return false;
    pub[0] = 4;
    bn_to_bytes(&x, C->limbs, pub + 1, (size_t)C->bytes);
    bn_to_bytes(&y, C->limbs, pub + 1 + C->bytes, (size_t)C->bytes);
    return true;
}

bool ec_ecdh(int curve, const uint8_t *priv, const uint8_t *peer_pub, size_t plen, uint8_t *shared)
{
    struct curve *C = get_curve(curve);
    if (!C) return false;
    pt_t p, r;
    if (!pt_parse(C, peer_pub, plen, &p)) return false;
    bn_t d, x;
    bn_from_bytes(&d, priv, (size_t)C->bytes);
    pt_mul(C, &r, &p, &d);
    if (!pt_affine(C, &r, &x, NULL)) return false;
    bn_to_bytes(&x, C->limbs, shared, (size_t)C->bytes);
    return true;
}

bool ecdsa_verify(int curve, const uint8_t *pub, size_t plen, const uint8_t *hash, size_t hlen,
                  const uint8_t *rb, size_t rlen, const uint8_t *sb, size_t slen)
{
    struct curve *C = get_curve(curve);
    if (!C) return false;
    const struct mont *N = &C->n;
    int nl = C->limbs;
    pt_t q;
    if (!pt_parse(C, pub, plen, &q)) return false;
    if (rlen > (size_t)C->bytes + 1 || slen > (size_t)C->bytes + 1) return false;
    bn_t r, s, e;
    bn_from_bytes(&r, rb, rlen);
    bn_from_bytes(&s, sb, slen);
    if (bn_is_zero(&r, nl) || bn_is_zero(&s, nl) || bn_cmp(&r, &N->m, nl) >= 0 || bn_cmp(&s, &N->m, nl) >= 0) return false;
    /* leftmost bits of the hash */
    bn_from_bytes(&e, hash, MIN(hlen, (size_t)C->bytes));
    mod_reduce(N, &e);
    bn_t sm, w, em, rm, u1m, u2m, u1, u2;
    mont_to(N, &sm, &s);
    mont_inv(N, &w, &sm);
    mont_to(N, &em, &e);
    mont_to(N, &rm, &r);
    mont_mul(N, &u1m, &em, &w);
    mont_mul(N, &u2m, &rm, &w);
    mont_from(N, &u1, &u1m);
    mont_from(N, &u2, &u2m);
    /* u1*G + u2*Q, Shamir's trick */
    pt_t g, gq, acc;
    generator(C, &g);
    pt_add(C, &gq, &g, &q);
    bn_zero(&acc.x);
    bn_zero(&acc.y);
    bn_zero(&acc.z);
    int bits = MAX(bn_bits(&u1, nl), bn_bits(&u2, nl));
    for (int i = bits - 1; i >= 0; i--) {
        pt_dbl(C, &acc, &acc);
        bool b1 = bn_bit(&u1, i), b2 = bn_bit(&u2, i);
        if (b1 && b2) pt_add(C, &acc, &acc, &gq);
        else if (b1) pt_add(C, &acc, &acc, &g);
        else if (b2) pt_add(C, &acc, &acc, &q);
    }
    bn_t x;
    if (!pt_affine(C, &acc, &x, NULL)) return false;
    mod_reduce(N, &x);
    return bn_cmp(&x, &r, nl) == 0;
}

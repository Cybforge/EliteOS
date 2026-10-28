/* Multi-precision arithmetic with Montgomery multiplication (CIOS). */
#include "bignum.h"

void bn_zero(bn_t *a) { memset(a, 0, sizeof(*a)); }

void bn_from_bytes(bn_t *a, const uint8_t *be, size_t len)
{
    bn_zero(a);
    while (len && !*be) { be++; len--; }
    if (len > BN_LIMBS * 4) { be += len - BN_LIMBS * 4; len = BN_LIMBS * 4; }
    for (size_t i = 0; i < len; i++) a->d[i / 4] |= (uint32_t)be[len - 1 - i] << (8 * (i % 4));
}

void bn_to_bytes(const bn_t *a, int n, uint8_t *be, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        size_t limb = i / 4;
        be[len - 1 - i] = limb < (size_t)n ? (uint8_t)(a->d[limb] >> (8 * (i % 4))) : 0;
    }
}

int bn_cmp(const bn_t *a, const bn_t *b, int n)
{
    for (int i = n - 1; i >= 0; i--) {
        if (a->d[i] > b->d[i]) return 1;
        if (a->d[i] < b->d[i]) return -1;
    }
    return 0;
}

bool bn_is_zero(const bn_t *a, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++) v |= a->d[i];
    return v == 0;
}

int bn_bits(const bn_t *a, int n)
{
    for (int i = n - 1; i >= 0; i--)
        if (a->d[i]) return i * 32 + 32 - __builtin_clz(a->d[i]);
    return 0;
}

bool bn_bit(const bn_t *a, int i) { return (a->d[i / 32] >> (i % 32)) & 1; }

/* r = a - b over n limbs, returns the borrow */
static uint32_t sub_n(uint32_t *r, const uint32_t *a, const uint32_t *b, int n)
{
    uint64_t borrow = 0;
    for (int i = 0; i < n; i++) {
        uint64_t t = (uint64_t)a[i] - b[i] - borrow;
        r[i] = (uint32_t)t;
        borrow = (t >> 32) & 1;
    }
    return (uint32_t)borrow;
}

static uint32_t add_n(uint32_t *r, const uint32_t *a, const uint32_t *b, int n)
{
    uint64_t carry = 0;
    for (int i = 0; i < n; i++) {
        uint64_t t = (uint64_t)a[i] + b[i] + carry;
        r[i] = (uint32_t)t;
        carry = t >> 32;
    }
    return (uint32_t)carry;
}

bool mont_init(struct mont *c, const bn_t *m, int n)
{
    if (n <= 0 || n > BN_LIMBS || !(m->d[0] & 1)) return false;
    memset(c, 0, sizeof(*c));
    c->n = n;
    c->m = *m;
    /* -m^-1 mod 2^32 by Newton iteration */
    uint32_t inv = 1;
    for (int i = 0; i < 5; i++) inv *= 2 - m->d[0] * inv;
    c->minv = (uint32_t)-inv;
    /* R mod m and R^2 mod m by repeated doubling of 1 */
    bn_t x;
    bn_zero(&x);
    x.d[0] = 1;
    for (int i = 0; i < 64 * n; i++) {
        uint32_t carry = add_n(x.d, x.d, x.d, n);
        if (carry || bn_cmp(&x, m, n) >= 0) sub_n(x.d, x.d, m->d, n);
        if (i == 32 * n - 1) c->one = x;
    }
    c->rr = x;
    return true;
}

void mont_mul(const struct mont *c, bn_t *r, const bn_t *a, const bn_t *b)
{
    int n = c->n;
    uint32_t t[BN_LIMBS + 2];
    memset(t, 0, sizeof(uint32_t) * (size_t)(n + 2));
    for (int i = 0; i < n; i++) {
        uint64_t carry = 0;
        uint32_t ai = a->d[i];
        for (int j = 0; j < n; j++) {
            uint64_t s = (uint64_t)t[j] + (uint64_t)ai * b->d[j] + carry;
            t[j] = (uint32_t)s;
            carry = s >> 32;
        }
        uint64_t s = (uint64_t)t[n] + carry;
        t[n] = (uint32_t)s;
        t[n + 1] = (uint32_t)(s >> 32);
        uint32_t mq = t[0] * c->minv;
        s = (uint64_t)t[0] + (uint64_t)mq * c->m.d[0];
        carry = s >> 32;
        for (int j = 1; j < n; j++) {
            s = (uint64_t)t[j] + (uint64_t)mq * c->m.d[j] + carry;
            t[j - 1] = (uint32_t)s;
            carry = s >> 32;
        }
        s = (uint64_t)t[n] + carry;
        t[n - 1] = (uint32_t)s;
        t[n] = t[n + 1] + (uint32_t)(s >> 32);
    }
    bn_t res;
    uint32_t borrow = sub_n(res.d, t, c->m.d, n);
    /* keep t if it was already below m */
    const uint32_t *src = (t[n] || !borrow) ? res.d : t;
    for (int i = 0; i < n; i++) r->d[i] = src[i];
    for (int i = n; i < BN_LIMBS; i++) r->d[i] = 0;
}

void mont_to(const struct mont *c, bn_t *r, const bn_t *a) { mont_mul(c, r, a, &c->rr); }

void mont_from(const struct mont *c, bn_t *r, const bn_t *a)
{
    bn_t one;
    bn_zero(&one);
    one.d[0] = 1;
    mont_mul(c, r, a, &one);
}

void mod_add(const struct mont *c, bn_t *r, const bn_t *a, const bn_t *b)
{
    bn_t t;
    uint32_t carry = add_n(t.d, a->d, b->d, c->n);
    bn_t u;
    uint32_t borrow = sub_n(u.d, t.d, c->m.d, c->n);
    const uint32_t *src = (carry || !borrow) ? u.d : t.d;
    for (int i = 0; i < c->n; i++) r->d[i] = src[i];
}

void mod_sub(const struct mont *c, bn_t *r, const bn_t *a, const bn_t *b)
{
    bn_t t;
    uint32_t borrow = sub_n(t.d, a->d, b->d, c->n);
    if (borrow) add_n(t.d, t.d, c->m.d, c->n);
    for (int i = 0; i < c->n; i++) r->d[i] = t.d[i];
}

void mod_reduce(const struct mont *c, bn_t *a)
{
    if (bn_cmp(a, &c->m, c->n) >= 0) sub_n(a->d, a->d, c->m.d, c->n);
}

void mont_pow(const struct mont *c, bn_t *r, const bn_t *base_m, const bn_t *e, int ebits)
{
    bn_t acc = c->one, b = *base_m;
    for (int i = ebits - 1; i >= 0; i--) {
        mont_mul(c, &acc, &acc, &acc);
        if (bn_bit(e, i)) mont_mul(c, &acc, &acc, &b);
    }
    *r = acc;
}

void mont_inv(const struct mont *c, bn_t *r, const bn_t *a_m)
{
    /* a^(m-2) for prime m */
    bn_t e = c->m, two;
    bn_zero(&two);
    two.d[0] = 2;
    sub_n(e.d, e.d, two.d, c->n);
    mont_pow(c, r, a_m, &e, bn_bits(&e, c->n));
}

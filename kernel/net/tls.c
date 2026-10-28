/*
 * TLS 1.3 client (RFC 8446).
 *
 * Cipher suites: TLS_AES_128_GCM_SHA256, TLS_CHACHA20_POLY1305_SHA256,
 * TLS_AES_256_GCM_SHA384. Key exchange: X25519, P-256, P-384 (with
 * HelloRetryRequest). Server authentication: X.509 chain up to a trusted
 * root, host name check, CertificateVerify with ECDSA or RSA-PSS.
 */
#include <tls.h>
#include <crypto.h>
#include <x509.h>
#include <mm.h>
#include <dev.h>

#define REC_MAX (16384 + 256)

enum { CT_CCS = 20, CT_ALERT = 21, CT_HANDSHAKE = 22, CT_APPDATA = 23 };
enum {
    HS_CLIENT_HELLO = 1, HS_SERVER_HELLO = 2, HS_NEW_SESSION_TICKET = 4, HS_ENCRYPTED_EXTENSIONS = 8,
    HS_CERTIFICATE = 11, HS_CERTIFICATE_REQUEST = 13, HS_CERTIFICATE_VERIFY = 15, HS_FINISHED = 20, HS_KEY_UPDATE = 24,
    HS_MESSAGE_HASH = 254,
};
enum { GROUP_X25519 = 0x001d, GROUP_P256 = 0x0017, GROUP_P384 = 0x0018 };

struct dir_keys {
    uint8_t secret[HASH_MAX];
    uint8_t key[32], iv[12];
    struct gcm gcm;
    uint64_t seq;
};

struct tls {
    tcp_sock_t *sock;
    int timeout;
    uint16_t suite;
    int hash, hlen, keylen;
    bool chacha;
    bool enc_in, enc_out;
    struct dir_keys rd, wr;
    /* handshake */
    uint8_t *tr;
    size_t tr_len, tr_cap;
    uint8_t *hs;
    size_t hs_len;
    /* incoming application data */
    uint8_t rec[REC_MAX + 5];
    uint8_t plain[REC_MAX];
    size_t plain_len, plain_off;
    bool eof;
    char *err;
    size_t errn;
    char desc[96], peer[96], issuer[96];
    uint16_t group;
};

static int fail(struct tls *t, const char *msg)
{
    if (t->err && !t->err[0]) strlcpy(t->err, msg, t->errn);
    return -1;
}

/* ------------------------------------------------------------------------
 * transcript and key schedule
 * ---------------------------------------------------------------------- */

static void tr_add(struct tls *t, const uint8_t *m, size_t n)
{
    if (t->tr_len + n > t->tr_cap) {
        t->tr_cap = MAX(t->tr_cap * 2, t->tr_len + n + 4096);
        t->tr = krealloc(t->tr, t->tr_cap);
    }
    memcpy(t->tr + t->tr_len, m, n);
    t->tr_len += n;
}

static void tr_hash(struct tls *t, uint8_t *out) { hash_once(t->hash, t->tr, t->tr_len, out); }

static void expand_label(struct tls *t, const uint8_t *secret, const char *label, const uint8_t *ctx, size_t clen,
                         uint8_t *out, size_t olen)
{
    uint8_t info[2 + 1 + 6 + 32 + 1 + HASH_MAX];
    size_t n = 0, ll = strlen(label);
    info[n++] = (uint8_t)(olen >> 8);
    info[n++] = (uint8_t)olen;
    info[n++] = (uint8_t)(6 + ll);
    memcpy(info + n, "tls13 ", 6);
    n += 6;
    memcpy(info + n, label, ll);
    n += ll;
    info[n++] = (uint8_t)clen;
    memcpy(info + n, ctx, clen);
    n += clen;
    hkdf_expand(t->hash, secret, info, n, out, olen);
}

static void derive_secret(struct tls *t, const uint8_t *secret, const char *label, const uint8_t *th, uint8_t *out)
{
    expand_label(t, secret, label, th, (size_t)t->hlen, out, (size_t)t->hlen);
}

static void set_keys(struct tls *t, struct dir_keys *k, const uint8_t *secret)
{
    memcpy(k->secret, secret, (size_t)t->hlen);
    expand_label(t, secret, "key", NULL, 0, k->key, (size_t)t->keylen);
    expand_label(t, secret, "iv", NULL, 0, k->iv, 12);
    if (!t->chacha) gcm_setkey(&k->gcm, k->key, t->keylen);
    k->seq = 0;
}

static void nonce(const struct dir_keys *k, uint8_t out[12])
{
    memcpy(out, k->iv, 12);
    for (int i = 0; i < 8; i++) out[4 + i] ^= (uint8_t)(k->seq >> (56 - 8 * i));
}

/* ------------------------------------------------------------------------
 * records
 * ---------------------------------------------------------------------- */

static int read_exact(struct tls *t, uint8_t *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        long r = tcp_recv(t->sock, buf + got, n - got, t->timeout);
        if (r == 0) return fail(t, "the server closed the connection");
        if (r == -2) return fail(t, "the server stopped answering");
        if (r < 0) return fail(t, "the connection was reset");
        got += (size_t)r;
    }
    return 0;
}

static int send_record(struct tls *t, int type, const uint8_t *data, size_t len)
{
    uint8_t *rec = kmalloc(len + 5 + 1 + 16);
    if (!rec) return -1;
    size_t n;
    if (!t->enc_out) {
        rec[0] = (uint8_t)type;
        rec[1] = 3;
        rec[2] = type == CT_HANDSHAKE && data[0] == HS_CLIENT_HELLO ? 1 : 3;
        rec[3] = (uint8_t)(len >> 8);
        rec[4] = (uint8_t)len;
        memcpy(rec + 5, data, len);
        n = len + 5;
    } else {
        size_t clen = len + 1 + 16;
        rec[0] = CT_APPDATA;
        rec[1] = 3;
        rec[2] = 3;
        rec[3] = (uint8_t)(clen >> 8);
        rec[4] = (uint8_t)clen;
        uint8_t *inner = kmalloc(len + 1);
        memcpy(inner, data, len);
        inner[len] = (uint8_t)type;
        uint8_t nn[12];
        nonce(&t->wr, nn);
        if (t->chacha) chachapoly_seal(t->wr.key, nn, rec, 5, inner, len + 1, rec + 5, rec + 5 + len + 1);
        else gcm_seal(&t->wr.gcm, nn, rec, 5, inner, len + 1, rec + 5, rec + 5 + len + 1);
        t->wr.seq++;
        kfree(inner);
        n = clen + 5;
    }
    long r = tcp_send(t->sock, rec, n);
    kfree(rec);
    return r == (long)n ? 0 : fail(t, "sending failed");
}

static const char *alert_name(int d)
{
    switch (d) {
    case 40: return "handshake failure";
    case 42: return "bad certificate";
    case 48: return "unknown CA";
    case 50: return "decode error";
    case 51: return "decrypt error";
    case 70: return "protocol version";
    case 71: return "insufficient security";
    case 80: return "internal error";
    case 109: return "missing extension";
    case 112: return "unrecognized name";
    case 120: return "no application protocol";
    }
    return "alert";
}

/* one record; handshake/app-data content goes to *out (decrypted if needed) */
static int read_record(struct tls *t, int *type, uint8_t **out, size_t *len)
{
    for (;;) {
        if (read_exact(t, t->rec, 5)) return -1;
        size_t n = (size_t)(t->rec[3] << 8 | t->rec[4]);
        if (n > REC_MAX) return fail(t, "the server sent an oversized record");
        if (read_exact(t, t->rec + 5, n)) return -1;
        int ct = t->rec[0];
        uint8_t *body = t->rec + 5;
        if (ct == CT_CCS) continue;                     /* middlebox compatibility, ignored */
        if (ct == CT_ALERT && !t->enc_in) {
            char m[80];
            snprintf(m, sizeof(m), "the server refused: %s", n >= 2 ? alert_name(body[1]) : "alert");
            return fail(t, m);
        }
        if (!t->enc_in) {
            *type = ct;
            *out = body;
            *len = n;
            return 0;
        }
        if (ct != CT_APPDATA || n < 17) return fail(t, "unexpected unprotected record");
        uint8_t nn[12];
        nonce(&t->rd, nn);
        size_t plen = n - 16;
        bool ok = t->chacha ? chachapoly_open(t->rd.key, nn, t->rec, 5, body, plen, t->plain, body + plen)
                            : gcm_open(&t->rd.gcm, nn, t->rec, 5, body, plen, t->plain, body + plen);
        if (!ok) return fail(t, "a record failed authentication");
        t->rd.seq++;
        while (plen && !t->plain[plen - 1]) plen--;     /* padding */
        if (!plen) return fail(t, "empty inner record");
        *type = t->plain[plen - 1];
        *out = t->plain;
        *len = plen - 1;
        if (*type == CT_ALERT) {
            if (*len >= 2 && t->plain[1] == 0) { t->eof = true; *len = 0; return 0; }     /* close_notify */
            char m[80];
            snprintf(m, sizeof(m), "the server sent an alert: %s", *len >= 2 ? alert_name(t->plain[1]) : "?");
            return fail(t, m);
        }
        return 0;
    }
}

/* next complete handshake message (header included) */
static int next_hs(struct tls *t, uint8_t **msg, size_t *mlen)
{
    for (;;) {
        if (t->hs_len >= 4) {
            size_t bl = (size_t)(t->hs[1] << 16 | t->hs[2] << 8 | t->hs[3]);
            if (bl > 65536 * 4) return fail(t, "handshake message too large");
            if (t->hs_len >= bl + 4) {
                *msg = kmalloc(bl + 4);
                memcpy(*msg, t->hs, bl + 4);
                *mlen = bl + 4;
                memmove(t->hs, t->hs + bl + 4, t->hs_len - bl - 4);
                t->hs_len -= bl + 4;
                return 0;
            }
        }
        int type;
        uint8_t *data;
        size_t len;
        if (read_record(t, &type, &data, &len)) return -1;
        if (t->eof) return fail(t, "the server closed the connection during the handshake");
        if (type != CT_HANDSHAKE) return fail(t, "expected a handshake message");
        t->hs = krealloc(t->hs, t->hs_len + len);
        memcpy(t->hs + t->hs_len, data, len);
        t->hs_len += len;
    }
}

/* ------------------------------------------------------------------------
 * handshake
 * ---------------------------------------------------------------------- */

static const uint8_t hrr_random[32] = {
    0xCF, 0x21, 0xAD, 0x74, 0xE5, 0x9A, 0x61, 0x11, 0xBE, 0x1D, 0x8C, 0x02, 0x1E, 0x65, 0xB8, 0x91,
    0xC2, 0xA2, 0x11, 0x16, 0x7A, 0xBB, 0x8C, 0x5E, 0x07, 0x9E, 0x09, 0xE2, 0xC8, 0xA8, 0x33, 0x9C,
};

struct keyshares {
    uint8_t x_priv[32], x_pub[32];
    uint8_t p256_priv[32], p256_pub[65];
    uint8_t p384_priv[48], p384_pub[97];
    bool have_p384;
};

struct buf { uint8_t *p; size_t n; };

static void put8(struct buf *b, int v) { b->p[b->n++] = (uint8_t)v; }
static void put16(struct buf *b, int v) { put8(b, v >> 8); put8(b, v); }
static void putbytes(struct buf *b, const void *d, size_t n) { memcpy(b->p + b->n, d, n); b->n += n; }
static void patch16(struct buf *b, size_t at) { size_t v = b->n - at - 2; b->p[at] = (uint8_t)(v >> 8); b->p[at + 1] = (uint8_t)v; }

static bool is_ip(const char *h)
{
    for (; *h; h++) if (!(*h == '.' || (*h >= '0' && *h <= '9'))) return false;
    return true;
}

static size_t build_client_hello(uint8_t *out, const uint8_t *random, const uint8_t *sid, const char *host,
                                 const struct keyshares *ks, uint16_t only_group, const uint8_t *cookie, size_t clen)
{
    struct buf b = { out, 0 };
    put8(&b, HS_CLIENT_HELLO);
    put8(&b, 0); put16(&b, 0);                  /* length, patched below */
    put16(&b, 0x0303);
    putbytes(&b, random, 32);
    put8(&b, 32);
    putbytes(&b, sid, 32);
    put16(&b, 6);
    put16(&b, 0x1301); put16(&b, 0x1303); put16(&b, 0x1302);
    put8(&b, 1); put8(&b, 0);
    size_t ext = b.n;
    put16(&b, 0);
    if (!is_ip(host)) {
        size_t hl = strlen(host);
        put16(&b, 0x0000);
        put16(&b, (int)hl + 5);
        put16(&b, (int)hl + 3);
        put8(&b, 0);
        put16(&b, (int)hl);
        putbytes(&b, host, hl);
    }
    put16(&b, 0x000a); put16(&b, 8); put16(&b, 6);
    put16(&b, GROUP_X25519); put16(&b, GROUP_P256); put16(&b, GROUP_P384);
    static const uint16_t sigalgs[] = { 0x0403, 0x0503, 0x0804, 0x0805, 0x0806, 0x0401, 0x0501, 0x0601 };
    put16(&b, 0x000d); put16(&b, 2 + 2 * (int)ARRAY_SIZE(sigalgs)); put16(&b, 2 * (int)ARRAY_SIZE(sigalgs));
    for (unsigned i = 0; i < ARRAY_SIZE(sigalgs); i++) put16(&b, sigalgs[i]);
    put16(&b, 0x002b); put16(&b, 3); put8(&b, 2); put16(&b, 0x0304);
    put16(&b, 0x002d); put16(&b, 2); put8(&b, 1); put8(&b, 1);
    put16(&b, 0x0010); put16(&b, 11); put16(&b, 9); put8(&b, 8); putbytes(&b, "http/1.1", 8);
    if (cookie) { put16(&b, 0x002c); put16(&b, (int)clen); putbytes(&b, cookie, clen); }
    put16(&b, 0x0033);
    size_t kse = b.n;
    put16(&b, 0);
    put16(&b, 0);
    if (!only_group || only_group == GROUP_X25519) { put16(&b, GROUP_X25519); put16(&b, 32); putbytes(&b, ks->x_pub, 32); }
    if (!only_group || only_group == GROUP_P256) { put16(&b, GROUP_P256); put16(&b, 65); putbytes(&b, ks->p256_pub, 65); }
    if (only_group == GROUP_P384) { put16(&b, GROUP_P384); put16(&b, 97); putbytes(&b, ks->p384_pub, 97); }
    patch16(&b, kse + 2);
    patch16(&b, kse);
    patch16(&b, ext);
    size_t body = b.n - 4;
    out[1] = (uint8_t)(body >> 16);
    out[2] = (uint8_t)(body >> 8);
    out[3] = (uint8_t)body;
    return b.n;
}

struct server_hello {
    bool hrr;
    uint16_t suite, version, group;
    const uint8_t *key;
    size_t key_len;
    const uint8_t *cookie;
    size_t cookie_len;
};

static int parse_server_hello(struct tls *t, const uint8_t *m, size_t n, struct server_hello *sh)
{
    memset(sh, 0, sizeof(*sh));
    if (n < 4 + 2 + 32 + 1) return fail(t, "short ServerHello");
    const uint8_t *p = m + 4, *end = m + n;
    p += 2;
    sh->hrr = !memcmp(p, hrr_random, 32);
    p += 32;
    size_t sidl = *p++;
    if (p + sidl + 3 > end) return fail(t, "bad ServerHello");
    p += sidl;
    sh->suite = (uint16_t)(p[0] << 8 | p[1]);
    p += 3;
    if (p + 2 > end) return fail(t, "the server does not speak TLS 1.3");
    size_t el = (size_t)(p[0] << 8 | p[1]);
    p += 2;
    if (p + el > end) return fail(t, "bad ServerHello extensions");
    const uint8_t *e = p, *ee = p + el;
    while (e + 4 <= ee) {
        int type = e[0] << 8 | e[1];
        size_t len = (size_t)(e[2] << 8 | e[3]);
        const uint8_t *v = e + 4;
        if (v + len > ee) break;
        if (type == 0x002b && len == 2) sh->version = (uint16_t)(v[0] << 8 | v[1]);
        if (type == 0x0033 && len >= 2) {
            sh->group = (uint16_t)(v[0] << 8 | v[1]);
            if (!sh->hrr && len >= 4) {
                sh->key_len = (size_t)(v[2] << 8 | v[3]);
                sh->key = v + 4;
                if (sh->key_len + 4 > len) return fail(t, "bad key share");
            }
        }
        if (type == 0x002c && len >= 2) {
            sh->cookie_len = (size_t)(v[0] << 8 | v[1]);
            sh->cookie = v + 2;
        }
        e = v + len;
    }
    if (sh->version != 0x0304) return fail(t, "the server does not support TLS 1.3");
    if (sh->suite != 0x1301 && sh->suite != 0x1302 && sh->suite != 0x1303) return fail(t, "no common cipher suite");
    return 0;
}

static int verify_certificate_msg(struct tls *t, const uint8_t *m, size_t n, const char *host, struct x509 **chain_out,
                                  uint8_t ***ders_out, int *count)
{
    const uint8_t *p = m + 4, *end = m + n;
    if (p >= end) return fail(t, "bad Certificate message");
    size_t ctxl = *p++;
    p += ctxl;
    if (p + 3 > end) return fail(t, "bad Certificate message");
    size_t listl = (size_t)(p[0] << 16 | p[1] << 8 | p[2]);
    p += 3;
    if (p + listl > end) return fail(t, "bad Certificate message");
    const uint8_t *le = p + listl;
    struct x509 *chain = kzalloc(sizeof(struct x509) * 10);
    uint8_t **ders = kzalloc(sizeof(uint8_t *) * 10);
    int nc = 0;
    while (p + 3 <= le && nc < 10) {
        size_t cl = (size_t)(p[0] << 16 | p[1] << 8 | p[2]);
        p += 3;
        if (p + cl + 2 > le) break;
        ders[nc] = kmalloc(cl);
        memcpy(ders[nc], p, cl);
        if (!x509_parse(ders[nc], cl, &chain[nc])) {
            kfree(ders[nc]);
            if (!nc) { kfree(chain); kfree(ders); return fail(t, "the server's certificate could not be read"); }
        } else {
            nc++;
        }
        p += cl;
        size_t xl = (size_t)(p[0] << 8 | p[1]);
        p += 2 + xl;
    }
    *chain_out = chain;
    *ders_out = ders;
    *count = nc;
    char err[160] = "";
    if (x509_verify_chain(chain, nc, host, rtc_epoch(), err, sizeof(err))) {
        char m2[200];
        snprintf(m2, sizeof(m2), "untrusted certificate: %s", err);
        return fail(t, m2);
    }
    strlcpy(t->peer, chain[0].cn[0] ? chain[0].cn : host, sizeof(t->peer));
    /* the issuer's CN is inside the issuer Name; find the matching certificate or root */
    t->issuer[0] = 0;
    for (int i = 1; i < nc; i++)
        if (chain[0].issuer_len == chain[i].subject_len && !memcmp(chain[0].issuer, chain[i].subject, chain[i].subject_len))
            strlcpy(t->issuer, chain[i].cn, sizeof(t->issuer));
    for (int i = 0; !t->issuer[0] && i < trust_count(); i++) {
        const struct x509 *r = trust_get(i);
        if (chain[0].issuer_len == r->subject_len && !memcmp(chain[0].issuer, r->subject, r->subject_len))
            strlcpy(t->issuer, r->cn, sizeof(t->issuer));
    }
    return 0;
}

static int check_certificate_verify(struct tls *t, const uint8_t *m, size_t n, const struct x509 *leaf,
                                    const uint8_t *th)
{
    if (n < 8) return fail(t, "bad CertificateVerify");
    int alg = m[4] << 8 | m[5];
    size_t sl = (size_t)(m[6] << 8 | m[7]);
    if (8 + sl > n) return fail(t, "bad CertificateVerify");
    uint8_t content[64 + 34 + HASH_MAX];
    memset(content, 0x20, 64);
    memcpy(content + 64, "TLS 1.3, server CertificateVerify", 33);
    content[97] = 0;
    memcpy(content + 98, th, (size_t)t->hlen);
    size_t cl = 98 + (size_t)t->hlen;
    int type, hash;
    switch (alg) {
    case 0x0403: type = SIG_ECDSA; hash = HASH_SHA256; break;
    case 0x0503: type = SIG_ECDSA; hash = HASH_SHA384; break;
    case 0x0603: type = SIG_ECDSA; hash = HASH_SHA512; break;
    case 0x0804: case 0x0809: type = SIG_RSA_PSS; hash = HASH_SHA256; break;
    case 0x0805: case 0x080a: type = SIG_RSA_PSS; hash = HASH_SHA384; break;
    case 0x0806: case 0x080b: type = SIG_RSA_PSS; hash = HASH_SHA512; break;
    default: return fail(t, "unsupported signature algorithm");
    }
    if (!x509_verify_with_key(leaf, type, hash, content, cl, m + 8, sl))
        return fail(t, "the server could not prove it owns the certificate");
    return 0;
}

struct tls *tls_connect(tcp_sock_t *s, const char *host, int timeout_ms, char *err, size_t errn)
{
    struct tls *t = kzalloc(sizeof(*t));
    if (!t) return NULL;
    t->sock = s;
    t->timeout = timeout_ms;
    t->err = err;
    t->errn = errn;
    if (err && errn) err[0] = 0;

    struct keyshares *ks = kzalloc(sizeof(*ks));
    random_bytes(ks->x_priv, 32);
    x25519_base(ks->x_pub, ks->x_priv);
    ec_keygen(CURVE_P256, ks->p256_priv, ks->p256_pub);
    uint8_t random[32], sid[32];
    random_bytes(random, 32);
    random_bytes(sid, 32);
    uint8_t *ch = kmalloc(2048);
    size_t chl = build_client_hello(ch, random, sid, host, ks, 0, NULL, 0);
    struct x509 *chain = NULL;
    uint8_t **ders = NULL;
    int nchain = 0;
    uint8_t *msg = NULL;
    size_t mlen;
    int rc = -1;
    struct server_hello sh;
    uint8_t shared[48];
    size_t shared_len = 0;

    if (send_record(t, CT_HANDSHAKE, ch, chl)) goto out;
    tr_add(t, ch, chl);
    if (next_hs(t, &msg, &mlen)) goto out;
    if (msg[0] != HS_SERVER_HELLO) { fail(t, "expected ServerHello"); goto out; }
    if (parse_server_hello(t, msg, mlen, &sh)) goto out;
    t->suite = sh.suite;
    t->hash = sh.suite == 0x1302 ? HASH_SHA384 : HASH_SHA256;
    t->hlen = hash_len(t->hash);
    if (sh.hrr) {
        /* HelloRetryRequest: the transcript restarts with a hash of the first ClientHello */
        uint8_t h1[HASH_MAX];
        hash_once(t->hash, ch, chl, h1);
        t->tr_len = 0;
        uint8_t mh[4] = { HS_MESSAGE_HASH, 0, 0, (uint8_t)t->hlen };
        tr_add(t, mh, 4);
        tr_add(t, h1, (size_t)t->hlen);
        tr_add(t, msg, mlen);
        if (sh.group == GROUP_P384) {
            ec_keygen(CURVE_P384, ks->p384_priv, ks->p384_pub);
            ks->have_p384 = true;
        } else if (sh.group != GROUP_X25519 && sh.group != GROUP_P256) {
            fail(t, "the server wants a key exchange group we do not have");
            goto out;
        }
        uint8_t *cookie = NULL;
        if (sh.cookie) {
            cookie = kmalloc(sh.cookie_len);
            memcpy(cookie, sh.cookie, sh.cookie_len);
        }
        uint16_t group = sh.group;
        size_t cookie_len = sh.cookie_len;
        kfree(msg);
        msg = NULL;
        kfree(ch);
        ch = kmalloc(2048 + cookie_len);
        chl = build_client_hello(ch, random, sid, host, ks, group, cookie, cookie_len);
        kfree(cookie);
        if (send_record(t, CT_HANDSHAKE, ch, chl)) goto out;
        tr_add(t, ch, chl);
        if (next_hs(t, &msg, &mlen)) goto out;
        if (msg[0] != HS_SERVER_HELLO) { fail(t, "expected ServerHello"); goto out; }
        if (parse_server_hello(t, msg, mlen, &sh) || sh.hrr) { fail(t, "second HelloRetryRequest"); goto out; }
    }
    tr_add(t, msg, mlen);
    t->chacha = sh.suite == 0x1303;
    t->keylen = sh.suite == 0x1301 ? 16 : 32;
    t->group = sh.group;
    /* shared secret */
    if (sh.group == GROUP_X25519 && sh.key_len == 32) {
        x25519(shared, ks->x_priv, sh.key);
        shared_len = 32;
        static const uint8_t zero[32];
        if (ct_equal(shared, zero, 32)) { fail(t, "invalid key share"); goto out; }
    } else if (sh.group == GROUP_P256 && ec_ecdh(CURVE_P256, ks->p256_priv, sh.key, sh.key_len, shared)) {
        shared_len = 32;
    } else if (sh.group == GROUP_P384 && ks->have_p384 && ec_ecdh(CURVE_P384, ks->p384_priv, sh.key, sh.key_len, shared)) {
        shared_len = 48;
    } else {
        fail(t, "the server's key share is not usable");
        goto out;
    }
    kfree(msg);
    msg = NULL;

    /* handshake secrets */
    uint8_t zeros[HASH_MAX] = { 0 }, early[HASH_MAX], derived[HASH_MAX], hs_secret[HASH_MAX], empty[HASH_MAX], th[HASH_MAX];
    uint8_t c_hs[HASH_MAX], s_hs[HASH_MAX];
    hkdf_extract(t->hash, NULL, 0, zeros, (size_t)t->hlen, early);
    hash_once(t->hash, "", 0, empty);
    derive_secret(t, early, "derived", empty, derived);
    hkdf_extract(t->hash, derived, (size_t)t->hlen, shared, shared_len, hs_secret);
    tr_hash(t, th);
    derive_secret(t, hs_secret, "c hs traffic", th, c_hs);
    derive_secret(t, hs_secret, "s hs traffic", th, s_hs);
    set_keys(t, &t->rd, s_hs);
    set_keys(t, &t->wr, c_hs);
    t->enc_in = true;

    bool cert_requested = false;
    uint8_t cert_req_ctx[256];
    size_t cert_req_ctx_len = 0;
    for (;;) {
        if (next_hs(t, &msg, &mlen)) goto out;
        int type = msg[0];
        if (type == HS_ENCRYPTED_EXTENSIONS) {
            tr_add(t, msg, mlen);
        } else if (type == HS_CERTIFICATE_REQUEST) {
            cert_requested = true;
            cert_req_ctx_len = mlen > 4 ? MIN((size_t)msg[4], sizeof(cert_req_ctx)) : 0;
            memcpy(cert_req_ctx, msg + 5, cert_req_ctx_len);
            tr_add(t, msg, mlen);
        } else if (type == HS_CERTIFICATE) {
            if (verify_certificate_msg(t, msg, mlen, host, &chain, &ders, &nchain)) goto out;
            tr_add(t, msg, mlen);
        } else if (type == HS_CERTIFICATE_VERIFY) {
            if (!nchain) { fail(t, "CertificateVerify without a certificate"); goto out; }
            tr_hash(t, th);
            if (check_certificate_verify(t, msg, mlen, &chain[0], th)) goto out;
            tr_add(t, msg, mlen);
        } else if (type == HS_FINISHED) {
            if (!nchain) { fail(t, "the server did not authenticate"); goto out; }
            uint8_t fk[HASH_MAX], expect[HASH_MAX];
            expand_label(t, s_hs, "finished", NULL, 0, fk, (size_t)t->hlen);
            tr_hash(t, th);
            hmac(t->hash, fk, (size_t)t->hlen, th, (size_t)t->hlen, expect);
            if (mlen != 4 + (size_t)t->hlen || !ct_equal(expect, msg + 4, (size_t)t->hlen)) {
                fail(t, "the server's Finished message is wrong");
                goto out;
            }
            tr_add(t, msg, mlen);
            break;
        } else {
            fail(t, "unexpected handshake message");
            goto out;
        }
        kfree(msg);
        msg = NULL;
    }
    kfree(msg);
    msg = NULL;

    /* application secrets from the transcript up to the server's Finished */
    uint8_t master[HASH_MAX], c_ap[HASH_MAX], s_ap[HASH_MAX];
    derive_secret(t, hs_secret, "derived", empty, derived);
    hkdf_extract(t->hash, derived, (size_t)t->hlen, zeros, (size_t)t->hlen, master);
    tr_hash(t, th);
    derive_secret(t, master, "c ap traffic", th, c_ap);
    derive_secret(t, master, "s ap traffic", th, s_ap);

    /* our side: compatibility CCS, an empty Certificate if asked, Finished */
    {
        static const uint8_t ccs[6] = { CT_CCS, 3, 3, 0, 1, 1 };
        tcp_send(t->sock, ccs, 6);
        t->enc_out = true;
        if (cert_requested) {
            uint8_t cm[4 + 1 + 255 + 3];
            size_t k = 0;
            cm[k++] = HS_CERTIFICATE;
            size_t bl = 1 + cert_req_ctx_len + 3;
            cm[k++] = 0; cm[k++] = (uint8_t)(bl >> 8); cm[k++] = (uint8_t)bl;
            cm[k++] = (uint8_t)cert_req_ctx_len;
            memcpy(cm + k, cert_req_ctx, cert_req_ctx_len);
            k += cert_req_ctx_len;
            cm[k++] = 0; cm[k++] = 0; cm[k++] = 0;
            if (send_record(t, CT_HANDSHAKE, cm, k)) goto out;
            tr_add(t, cm, k);
        }
        uint8_t fk[HASH_MAX], fin[4 + HASH_MAX];
        expand_label(t, c_hs, "finished", NULL, 0, fk, (size_t)t->hlen);
        tr_hash(t, th);
        fin[0] = HS_FINISHED;
        fin[1] = 0; fin[2] = 0; fin[3] = (uint8_t)t->hlen;
        hmac(t->hash, fk, (size_t)t->hlen, th, (size_t)t->hlen, fin + 4);
        if (send_record(t, CT_HANDSHAKE, fin, 4 + (size_t)t->hlen)) goto out;
    }
    set_keys(t, &t->rd, s_ap);
    set_keys(t, &t->wr, c_ap);
    const char *suite = t->suite == 0x1301 ? "AES-128-GCM" : t->suite == 0x1302 ? "AES-256-GCM" : "ChaCha20-Poly1305";
    const char *grp = t->group == GROUP_X25519 ? "X25519" : t->group == GROUP_P256 ? "P-256" : "P-384";
    snprintf(t->desc, sizeof(t->desc), "TLS 1.3 · %s · %s", suite, grp);
    rc = 0;
out:
    kfree(msg);
    kfree(ch);
    kfree(ks);
    for (int i = 0; ders && i < nchain; i++) kfree(ders[i]);
    kfree(ders);
    kfree(chain);
    kfree(t->tr);
    t->tr = NULL;
    t->tr_len = t->tr_cap = 0;
    memset(shared, 0, sizeof(shared));
    if (rc) {
        if (t->enc_out || t->enc_in) {} /* nothing to send: the server already knows */
        kfree(t->hs);
        kfree(t);
        return NULL;
    }
    klog("tls: %s: %s, certificate '%s' issued by '%s'", host, t->desc, t->peer, t->issuer);
    return t;
}

/* ------------------------------------------------------------------------
 * application data
 * ---------------------------------------------------------------------- */

long tls_send(struct tls *t, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    size_t done = 0;
    while (done < len) {
        size_t k = MIN(len - done, (size_t)16384);
        if (send_record(t, CT_APPDATA, p + done, k)) return done ? (long)done : -1;
        done += k;
    }
    return (long)done;
}

static void key_update_in(struct tls *t)
{
    uint8_t next[HASH_MAX];
    expand_label(t, t->rd.secret, "traffic upd", NULL, 0, next, (size_t)t->hlen);
    set_keys(t, &t->rd, next);
}

long tls_recv(struct tls *t, void *buf, size_t len, int timeout_ms)
{
    t->timeout = timeout_ms;
    for (;;) {
        if (t->plain_off < t->plain_len) {
            size_t k = MIN(len, t->plain_len - t->plain_off);
            memcpy(buf, t->plain + t->plain_off, k);
            t->plain_off += k;
            return (long)k;
        }
        if (t->eof) return 0;
        char e[96] = "";
        char *saved = t->err;
        size_t savedn = t->errn;
        t->err = e;
        t->errn = sizeof(e);
        int type;
        uint8_t *data;
        size_t dl;
        int r = read_record(t, &type, &data, &dl);
        t->err = saved;
        t->errn = savedn;
        if (r) return strstr(e, "closed") ? 0 : -1;
        if (t->eof) return 0;
        if (type == CT_APPDATA) {
            /* data already sits at the start of t->plain */
            t->plain_off = 0;
            t->plain_len = dl;
            continue;
        }
        if (type == CT_HANDSHAKE) {
            /* post-handshake: session tickets are ignored, key updates honoured */
            for (size_t o = 0; o + 4 <= dl;) {
                size_t bl = (size_t)(data[o + 1] << 16 | data[o + 2] << 8 | data[o + 3]);
                if (data[o] == HS_KEY_UPDATE && bl >= 1) {
                    bool request = data[o + 4] == 1;
                    key_update_in(t);
                    if (request) {
                        uint8_t ku[5] = { HS_KEY_UPDATE, 0, 0, 1, 0 };
                        send_record(t, CT_HANDSHAKE, ku, 5);
                        uint8_t next[HASH_MAX];
                        expand_label(t, t->wr.secret, "traffic upd", NULL, 0, next, (size_t)t->hlen);
                        set_keys(t, &t->wr, next);
                    }
                }
                o += 4 + bl;
            }
            t->plain_len = t->plain_off = 0;
            continue;
        }
        return -1;
    }
}

void tls_close(struct tls *t)
{
    if (!t) return;
    uint8_t alert[2] = { 1, 0 };            /* close_notify */
    send_record(t, CT_ALERT, alert, 2);
    kfree(t->hs);
    memset(t, 0, sizeof(*t));
    kfree(t);
}

const char *tls_description(struct tls *t) { return t->desc; }
const char *tls_peer_name(struct tls *t) { return t->peer; }
const char *tls_issuer_name(struct tls *t) { return t->issuer; }

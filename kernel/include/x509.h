#ifndef ZENITH_X509_H
#define ZENITH_X509_H

#include <kernel.h>

enum { SIG_UNKNOWN, SIG_RSA_PKCS1, SIG_RSA_PSS, SIG_ECDSA };
enum { KEY_UNKNOWN, KEY_RSA, KEY_EC };

struct x509 {
    const uint8_t *raw;             /* the whole certificate (DER) */
    size_t raw_len;
    const uint8_t *tbs;             /* signed part, header included */
    size_t tbs_len;
    int sig_type, sig_hash;         /* SIG_*, HASH_* */
    const uint8_t *sig;
    size_t sig_len;
    const uint8_t *issuer, *subject;    /* DER Names, compared byte for byte */
    size_t issuer_len, subject_len;
    int64_t not_before, not_after;
    int key_type, curve;
    const uint8_t *rsa_n, *rsa_e, *ec_point;
    size_t rsa_n_len, rsa_e_len, ec_point_len;
    bool is_ca;
    const uint8_t *san;             /* SubjectAltName GeneralNames */
    size_t san_len;
    char cn[96];
};

bool x509_parse(const uint8_t *der, size_t len, struct x509 *c);
bool x509_signed_by(const struct x509 *cert, const struct x509 *issuer);
bool x509_match_host(const struct x509 *c, const char *host);
/* 0 on success; err gets a readable reason otherwise */
int  x509_verify_chain(const struct x509 *chain, int n, const char *host, int64_t now, char *err, size_t errn);

/* signatures made with a certificate's key (TLS CertificateVerify) */
bool x509_verify_with_key(const struct x509 *c, int sig_type, int hash_alg, const uint8_t *msg, size_t mlen,
                          const uint8_t *sig, size_t slen);

/* trust store: Mozilla roots built in, plus PEM files in /etc/ssl/certs */
int  trust_count(void);
const struct x509 *trust_get(int i);
void trust_reload(void);

int  pem_decode(const char *pem, size_t len, uint8_t **ders, size_t *lens, int max);   /* kmalloc'd */

#endif

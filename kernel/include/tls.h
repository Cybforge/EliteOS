#ifndef ZENITH_TLS_H
#define ZENITH_TLS_H

#include <net.h>

/* A TLS 1.3 client connection on top of a TCP socket (RFC 8446). */
struct tls;

struct tls *tls_connect(tcp_sock_t *s, const char *host, int timeout_ms, char *err, size_t errn);
long tls_send(struct tls *t, const void *buf, size_t len);
long tls_recv(struct tls *t, void *buf, size_t len, int timeout_ms);    /* 0 = closed, <0 error */
void tls_close(struct tls *t);                                          /* frees t; the socket stays open */
const char *tls_description(struct tls *t);                             /* "TLS 1.3 · AES-128-GCM · X25519" */
const char *tls_peer_name(struct tls *t);                               /* certificate subject CN */
const char *tls_issuer_name(struct tls *t);

#endif

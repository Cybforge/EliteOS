#ifndef ZENITH_NET_H
#define ZENITH_NET_H

#include <kernel.h>

struct net_info {
    char ifname[8];                         /* eth0, eth1, usb0 ... */
    char driver[48];                        /* adapter model */
    char drv[16];                           /* driver name, e.g. "e1000" */
    uint8_t mac[6];
    bool link;
    bool is_default;                        /* carries the default route */
    uint32_t ip, netmask, gateway, dns;     /* host byte order */
    uint64_t rx_packets, tx_packets, rx_bytes, tx_bytes, rx_dropped;
};

/* the interface that carries the default route (or the first one) */
bool net_get_info(struct net_info *ni);
int  net_iface_count(void);
bool net_iface_get(int i, struct net_info *ni);
int  net_card_list(void (*cb)(void *ctx, const char *driver, const char *family, uint16_t vendor, uint16_t device,
                              const char *model),
                   void *ctx);
bool net_udp_request(uint32_t dst, uint16_t dport, const void *req, size_t rlen, void *resp, size_t *rn, int timeout_ms);

/* network time (SNTP, net/ntp.c) */
struct ntp_status {
    bool enabled, synced;
    char server[64];
    int64_t last_offset;                    /* seconds the clock was off at the last sync */
    int64_t last_sync;                      /* UTC time of the last sync */
    char error[64];
};
void ntp_status(struct ntp_status *out);
void ntp_sync_now(void);
void ntp_init(void);                    /* wake the sync thread */

bool net_dhcp(int timeout_ms);              /* every interface with a link */
bool net_resolve(const char *host, uint32_t *ip, int timeout_ms);
int  net_ping(uint32_t ip, uint16_t seq, int timeout_ms);          /* rtt ms, -1 on timeout */
int  net_http_get(const char *url, char **body, size_t *len, int timeout_ms);  /* status or -1 */
int  net_http_get_ex(const char *url, char **body, size_t *len, char *location, size_t locn, int timeout_ms);
void ip_to_str(uint32_t ip, char *out);
bool net_route_ready(uint32_t dst, int timeout_ms);

/* TCP client sockets */
typedef struct tcp_sock tcp_sock_t;
tcp_sock_t *tcp_connect(uint32_t ip, uint16_t port, int timeout_ms);
long tcp_send(tcp_sock_t *s, const void *data, size_t len);
long tcp_recv(tcp_sock_t *s, void *buf, size_t len, int timeout_ms);   /* 0 = closed, -1 error, -2 timeout */
void tcp_close(tcp_sock_t *s);
int  tcp_open_count(void);

/* HTTP(S) requests */
struct http_result {
    int status;                 /* -1 when there was no answer */
    char *body;                 /* kmalloc'd, NUL terminated */
    size_t len;
    char location[512];         /* redirect target */
    char content_type[96];
    char error[160];            /* human readable reason on failure */
    char tls[96];               /* e.g. "TLS 1.3, AES-128-GCM, X25519" when secure */
};
int  net_http_request(const char *url, struct http_result *r, int timeout_ms);
bool str_to_ip(const char *s, uint32_t *ip);

#endif

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

/* the same for the socket layer: nothing here sleeps */
#define TCPF_READ       0x01        /* data to read */
#define TCPF_EOF        0x02        /* the peer is done sending (or the connection is gone) */
#define TCPF_WRITE      0x04        /* connected and there is room to send */
#define TCPF_ERR        0x08        /* reset or timed out */
#define TCPF_CONNECTING 0x10
#define TCPF_CLOSED     0x20
#define TCPF_TIMEDOUT   0x40
tcp_sock_t *tcp_connect_start(uint32_t ip, uint16_t port);
uint32_t tcp_flags(tcp_sock_t *s);
long   tcp_send_nb(tcp_sock_t *s, const void *data, size_t len);      /* bytes taken (0: full), -1 on error */
size_t tcp_readable(tcp_sock_t *s);
size_t tcp_unsent(tcp_sock_t *s);
void   tcp_shutdown_write(tcp_sock_t *s);
void   tcp_release(tcp_sock_t *s);                                      /* close and let the stack finish it */
void   tcp_addrs(tcp_sock_t *s, uint32_t *lip, uint16_t *lport, uint32_t *rip, uint16_t *rport);
typedef struct tcp_listener tcp_listener_t;
tcp_listener_t *tcp_listen(uint16_t port, int backlog);              /* NULL when the port is taken */
tcp_sock_t *tcp_accept_nb(tcp_listener_t *l);
bool   tcp_listener_ready(tcp_listener_t *l);
void   tcp_listen_close(tcp_listener_t *l);

/* called whenever a socket may have become readable or writable; the socket layer wakes its sleepers from it */
extern void (*net_activity_hook)(void);

/* UDP endpoints: bound port, queue of received datagrams, loopback inside the host */
typedef struct udp_ep udp_ep_t;
udp_ep_t *udp_ep_open(uint16_t port);                                  /* 0: pick a free port; NULL when taken */
uint16_t  udp_ep_port(udp_ep_t *e);
long      udp_ep_send(udp_ep_t *e, uint32_t dst, uint16_t dport, const void *data, size_t len);   /* bytes, -1 on failure */
long      udp_ep_recv(udp_ep_t *e, void *buf, size_t cap, uint32_t *from, uint16_t *fport, bool peek);   /* -2: nothing queued */
bool      udp_ep_ready(udp_ep_t *e);
void      udp_ep_close(udp_ep_t *e);
bool      net_is_local_ip(uint32_t ip);                                /* 127.0.0.0/8 or one of our addresses */
uint32_t  net_src_ip(uint32_t dst);                                      /* our address on the way to dst (0: no route) */

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

/* download to a file, written as it arrives; 0 on success, -1 with a readable message in err */
typedef void (*net_progress_fn)(void *ctx, uint64_t done, uint64_t total);   /* total 0 = unknown */
int  net_http_download(const char *url, const char *dest, uint64_t max_bytes, net_progress_fn cb, void *ctx,
                       char *err, size_t errn);
bool str_to_ip(const char *s, uint32_t *ip);

#endif

/*
 * The TCP/IP stack's lower half: Ethernet, ARP, IPv4, ICMP echo, UDP,
 * DHCP client and DNS resolver. TCP lives in tcp.c, HTTP(S) in http.c.
 *
 * Any number of adapters can be up at once (eth0, eth1, usb0 ...), each
 * with its own DHCP lease and ARP cache. Traffic for a local subnet leaves
 * through the adapter on that subnet, everything else through the first
 * adapter with a link and a gateway, so unplugging one cable (or a phone)
 * moves traffic to the next adapter by itself. One network thread polls
 * every adapter; a second one keeps the leases up to date.
 */
#include <kernel.h>
#include <net.h>
#include <sched.h>
#include <dev.h>
#include <mm.h>
#include <vfs.h>
#include <x86.h>
#include <crypto.h>
#include <sys.h>
#include "netdev.h"

#define MAX_IF 8

struct arp_entry { uint32_t ip; uint8_t mac[6]; uint64_t t; };

struct iface {
    struct netdev *nd;
    struct net_info info;
    struct arp_entry arp[16];
    uint64_t dhcp_next;         /* earliest time for the next DHCP attempt */
    int dhcp_fails;
    uint64_t link_checked;
};

static struct iface ifs[MAX_IF];
static volatile int nifs;
static spinlock_t net_lock = SPINLOCK_INIT("net");
static spinlock_t if_lock = SPINLOCK_INIT("netif");
static volatile uint64_t last_rx_t;
static char netcfg_chan;

#define ETH_IP  0x0800
#define ETH_ARP 0x0806

static inline uint16_t htons(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }
static inline uint32_t htonl(uint32_t v)
{
    return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00) | (v >> 24);
}
#define ntohs htons
#define ntohl htonl

struct PACKED eth { uint8_t dst[6], src[6]; uint16_t type; };
struct PACKED arp {
    uint16_t htype, ptype;
    uint8_t hlen, plen;
    uint16_t op;
    uint8_t sha[6];
    uint32_t spa;
    uint8_t tha[6];
    uint32_t tpa;
};
struct PACKED ip4 {
    uint8_t vihl, tos;
    uint16_t len, id, frag;
    uint8_t ttl, proto;
    uint16_t csum;
    uint32_t src, dst;
};
struct PACKED icmp { uint8_t type, code; uint16_t csum, id, seq; };
struct PACKED udp { uint16_t sport, dport, len, csum; };
struct PACKED tcp {
    uint16_t sport, dport;
    uint32_t seq, ack;
    uint8_t off, flags;
    uint16_t win, csum, urg;
};

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

static const uint8_t bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

void ip_to_str(uint32_t ip, char *out)
{
    snprintf(out, 16, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
}

bool str_to_ip(const char *s, uint32_t *ip)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        if (!isdigit(*s)) return false;
        int n = 0;
        while (isdigit(*s)) n = n * 10 + (*s++ - '0');
        if (n > 255) return false;
        v = (v << 8) | (uint32_t)n;
        if (i < 3 && *s++ != '.') return false;
    }
    if (*s) return false;
    *ip = v;
    return true;
}

uint16_t net_csum(const void *data, size_t len, uint32_t sum)
{
    const uint8_t *p = data;
    for (size_t i = 0; i + 1 < len; i += 2) sum += (uint32_t)(p[i] << 8 | p[i + 1]);
    if (len & 1) sum += (uint32_t)p[len - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return htons((uint16_t)~sum);
}

/* ------------------------------------------------------------------------
 * link layer
 * ---------------------------------------------------------------------- */

static bool eth_send(struct iface *ifc, const uint8_t *dst, uint16_t type, const void *payload, size_t len)
{
    uint8_t frame[1600];
    if (len > 1500 || ifc->nd->gone) return false;
    struct eth *e = (struct eth *)frame;
    memcpy(e->dst, dst, 6);
    memcpy(e->src, ifc->nd->mac, 6);
    e->type = htons(type);
    memcpy(frame + sizeof(*e), payload, len);
    size_t total = sizeof(*e) + len;
    if (total < 60) { memset(frame + total, 0, 60 - total); total = 60; }
    bool ok = ifc->nd->send(ifc->nd, frame, total);
    if (ok) { ifc->info.tx_packets++; ifc->info.tx_bytes += total; }
    return ok;
}

/* ARP cache, one per interface */
static char arp_chan;

static bool arp_lookup(struct iface *ifc, uint32_t ip, uint8_t *mac)
{
    bool ok = false;
    spin_lock(&net_lock);
    for (int i = 0; i < 16; i++)
        if (ifc->arp[i].ip == ip && ifc->arp[i].t) { memcpy(mac, ifc->arp[i].mac, 6); ok = true; }
    spin_unlock(&net_lock);
    return ok;
}

static void arp_store(struct iface *ifc, uint32_t ip, const uint8_t *mac)
{
    spin_lock(&net_lock);
    int slot = 0;
    uint64_t oldest = ~0ull;
    for (int i = 0; i < 16; i++) {
        if (ifc->arp[i].ip == ip) { slot = i; break; }
        if (ifc->arp[i].t < oldest) { oldest = ifc->arp[i].t; slot = i; }
    }
    ifc->arp[slot].ip = ip;
    memcpy(ifc->arp[slot].mac, mac, 6);
    ifc->arp[slot].t = uptime_ms() | 1;
    spin_unlock(&net_lock);
    sched_wake(&arp_chan);
}

static void arp_send(struct iface *ifc, uint16_t op, const uint8_t *tha, uint32_t tpa)
{
    struct arp a;
    a.htype = htons(1);
    a.ptype = htons(ETH_IP);
    a.hlen = 6;
    a.plen = 4;
    a.op = htons(op);
    memcpy(a.sha, ifc->nd->mac, 6);
    a.spa = htonl(ifc->info.ip);
    memcpy(a.tha, tha, 6);
    a.tpa = htonl(tpa);
    eth_send(ifc, op == 1 ? bcast : tha, ETH_ARP, &a, sizeof(a));
}

/* ------------------------------------------------------------------------
 * routing
 * ---------------------------------------------------------------------- */

static bool if_up(struct iface *ifc)
{
    return ifc->nd && !ifc->nd->gone && ifc->info.link && ifc->info.ip;
}

static struct iface *default_iface(void)
{
    for (int i = 0; i < nifs; i++)
        if (if_up(&ifs[i]) && ifs[i].info.gateway) return &ifs[i];
    for (int i = 0; i < nifs; i++)
        if (if_up(&ifs[i])) return &ifs[i];
    return NULL;
}

/* the interface a packet to `dst` leaves through, and the next hop on it */
static struct iface *route(uint32_t dst, uint32_t *nexthop)
{
    for (int i = 0; i < nifs; i++) {
        struct iface *ifc = &ifs[i];
        if (if_up(ifc) && ifc->info.netmask && (dst & ifc->info.netmask) == (ifc->info.ip & ifc->info.netmask)) {
            *nexthop = dst;
            return ifc;
        }
    }
    struct iface *ifc = default_iface();
    if (ifc) *nexthop = ifc->info.gateway ? ifc->info.gateway : dst;
    return ifc;
}

static bool arp_resolve(struct iface *ifc, uint32_t ip, uint8_t *mac, int timeout_ms)
{
    if (ip == 0xFFFFFFFF) { memcpy(mac, bcast, 6); return true; }
    if (arp_lookup(ifc, ip, mac)) return true;
    if (!timeout_ms) {
        /* callers that must not sleep: ask, and let a retransmission find the answer */
        arp_send(ifc, 1, (const uint8_t *)"\0\0\0\0\0\0", ip);
        return false;
    }
    uint64_t end = uptime_ms() + (uint64_t)timeout_ms;
    while (uptime_ms() < end) {
        if (arp_lookup(ifc, ip, mac)) return true;
        arp_send(ifc, 1, (const uint8_t *)"\0\0\0\0\0\0", ip);
        sched_wait(&arp_chan, NULL, 200);
    }
    return arp_lookup(ifc, ip, mac);
}

/* ------------------------------------------------------------------------
 * IPv4
 * ---------------------------------------------------------------------- */

static uint16_t ip_id = 1;

static bool ip_output(struct iface *ifc, const uint8_t *mac, uint32_t dst, uint8_t proto, const void *payload, size_t len)
{
    uint8_t pkt[1500];
    if (len + sizeof(struct ip4) > sizeof(pkt)) return false;
    struct ip4 *h = (struct ip4 *)pkt;
    h->vihl = 0x45;
    h->tos = 0;
    h->len = htons((uint16_t)(sizeof(*h) + len));
    h->id = htons(__atomic_fetch_add(&ip_id, 1, __ATOMIC_RELAXED));
    h->frag = htons(0x4000);
    h->ttl = 64;
    h->proto = proto;
    h->csum = 0;
    h->src = htonl(ifc->info.ip);
    h->dst = htonl(dst);
    h->csum = net_csum(h, sizeof(*h), 0);
    memcpy(pkt + sizeof(*h), payload, len);
    return eth_send(ifc, mac, ETH_IP, pkt, sizeof(*h) + len);
}

static bool ip_send_ex(uint32_t dst, uint8_t proto, const void *payload, size_t len, int arp_ms)
{
    uint32_t hop;
    struct iface *ifc = route(dst, &hop);
    uint8_t mac[6];
    if (!ifc || !arp_resolve(ifc, hop, mac, arp_ms)) return false;
    return ip_output(ifc, mac, dst, proto, payload, len);
}

static bool ip_send(uint32_t dst, uint8_t proto, const void *payload, size_t len)
{
    return ip_send_ex(dst, proto, payload, len, 1500);
}

/* never sleeps (TCP calls it with its lock held) */
bool net_ip_send(uint32_t dst, uint8_t proto, const void *payload, size_t len)
{
    return ip_send_ex(dst, proto, payload, len, 0);
}

bool net_route_ready(uint32_t dst, int timeout_ms)
{
    uint32_t hop;
    struct iface *ifc = route(dst, &hop);
    uint8_t mac[6];
    return ifc && arp_resolve(ifc, hop, mac, timeout_ms);
}

/* the source address for packets to `dst` */
uint32_t net_src_ip(uint32_t dst)
{
    uint32_t hop;
    struct iface *ifc = route(dst, &hop);
    return ifc ? ifc->info.ip : 0;
}

/* the largest TCP window the adapter towards `dst` can take in one burst */
uint32_t net_rx_window(uint32_t dst)
{
    uint32_t hop;
    struct iface *ifc = route(dst, &hop);
    return ifc && ifc->nd->rx_window ? ifc->nd->rx_window : 65535;
}

/* ------------------------------------------------------------------------
 * ICMP
 * ---------------------------------------------------------------------- */

static volatile uint16_t ping_wait_seq;
static volatile bool ping_got;
static char ping_chan;

static void icmp_input(uint32_t src, uint8_t *data, size_t len)
{
    if (len < sizeof(struct icmp)) return;
    struct icmp *ic = (struct icmp *)data;
    if (ic->type == 8) {                     /* echo request -> reply */
        ic->type = 0;
        ic->csum = 0;
        ic->csum = net_csum(data, len, 0);
        ip_send(src, 1, data, len);
    } else if (ic->type == 0 && ntohs(ic->id) == 0x5A4E && ntohs(ic->seq) == ping_wait_seq) {
        ping_got = true;
        sched_wake(&ping_chan);
    }
}

int net_ping(uint32_t ip, uint16_t seq, int timeout_ms)
{
    uint8_t buf[sizeof(struct icmp) + 32];
    struct icmp *ic = (struct icmp *)buf;
    ic->type = 8;
    ic->code = 0;
    ic->id = htons(0x5A4E);
    ic->seq = htons(seq);
    for (int i = 0; i < 32; i++) buf[sizeof(*ic) + i] = (uint8_t)('a' + i % 26);
    ic->csum = 0;
    ic->csum = net_csum(buf, sizeof(buf), 0);
    ping_wait_seq = seq;
    ping_got = false;
    uint64_t t0 = uptime_ms();
    if (!ip_send(ip, 1, buf, sizeof(buf))) return -1;
    while (!ping_got && uptime_ms() - t0 < (uint64_t)timeout_ms) sched_wait(&ping_chan, NULL, 50);
    return ping_got ? (int)(uptime_ms() - t0) : -1;
}

/* ------------------------------------------------------------------------
 * UDP (one waiting receiver per port)
 * ---------------------------------------------------------------------- */

struct udp_sock {
    uint16_t port;
    struct iface *ifc;          /* only accept datagrams from this interface (DHCP) */
    uint8_t buf[1500];
    volatile int len;
    uint32_t from;
};
static struct udp_sock *udp_socks[MAX_IF + 4];
static char udp_chan;

static size_t udp_build(uint8_t *pkt, uint16_t sport, uint16_t dport, const void *data, size_t len)
{
    struct udp *u = (struct udp *)pkt;
    u->sport = htons(sport);
    u->dport = htons(dport);
    u->len = htons((uint16_t)(sizeof(*u) + len));
    u->csum = 0;
    memcpy(pkt + sizeof(*u), data, len);
    return sizeof(*u) + len;
}

static bool udp_send(uint32_t dst, uint16_t sport, uint16_t dport, const void *data, size_t len)
{
    uint8_t pkt[1472];
    if (len + sizeof(struct udp) > sizeof(pkt)) return false;
    return ip_send(dst, 17, pkt, udp_build(pkt, sport, dport, data, len));
}

/* link-level broadcast out of one interface (DHCP) */
static bool udp_broadcast(struct iface *ifc, uint16_t sport, uint16_t dport, const void *data, size_t len)
{
    uint8_t pkt[1472];
    if (len + sizeof(struct udp) > sizeof(pkt)) return false;
    return ip_output(ifc, bcast, 0xFFFFFFFF, 17, pkt, udp_build(pkt, sport, dport, data, len));
}

static void udp_input(struct iface *ifc, uint32_t src, uint8_t *data, size_t len)
{
    if (len < sizeof(struct udp)) return;
    struct udp *u = (struct udp *)data;
    uint16_t port = ntohs(u->dport);
    size_t ul = ntohs(u->len);
    if (ul < sizeof(*u) || ul > len) return;
    size_t plen = ul - sizeof(*u);
    spin_lock(&net_lock);
    for (size_t i = 0; i < ARRAY_SIZE(udp_socks); i++) {
        struct udp_sock *s = udp_socks[i];
        if (s && s->port == port && !s->len && (!s->ifc || s->ifc == ifc)) {
            memcpy(s->buf, data + sizeof(*u), MIN(plen, sizeof(s->buf)));
            s->from = src;
            s->len = (int)MIN(plen, sizeof(s->buf));
        }
    }
    spin_unlock(&net_lock);
    sched_wake(&udp_chan);
}

static bool udp_bind(struct udp_sock *s, uint16_t port, struct iface *ifc)
{
    s->port = port;
    s->ifc = ifc;
    s->len = 0;
    bool ok = false;
    spin_lock(&net_lock);
    for (size_t i = 0; i < ARRAY_SIZE(udp_socks) && !ok; i++)
        if (!udp_socks[i]) { udp_socks[i] = s; ok = true; }
    spin_unlock(&net_lock);
    return ok;
}

static void udp_unbind(struct udp_sock *s)
{
    spin_lock(&net_lock);
    for (size_t i = 0; i < ARRAY_SIZE(udp_socks); i++) if (udp_socks[i] == s) udp_socks[i] = NULL;
    spin_unlock(&net_lock);
}

static bool udp_wait(struct udp_sock *s, uint64_t deadline)
{
    while (!s->len && uptime_ms() < deadline) sched_wait(&udp_chan, NULL, 50);
    return s->len > 0;
}

/* one UDP question and its answer (NTP): sends from a random port, waits for a datagram back */
bool net_udp_request(uint32_t dst, uint16_t dport, const void *req, size_t rlen, void *resp, size_t *rn, int timeout_ms)
{
    struct udp_sock *s = kzalloc(sizeof(*s));
    if (!s) return false;
    uint16_t r;
    random_bytes(&r, sizeof(r));
    uint16_t port = (uint16_t)(30000 + (r % 30000));
    if (!udp_bind(s, port, NULL)) { kfree(s); return false; }
    bool ok = udp_send(dst, port, dport, req, rlen);
    if (ok) {
        uint64_t deadline = uptime_ms() + (uint64_t)timeout_ms;
        ok = udp_wait(s, deadline);
        while (ok && s->from != dst) {                      /* ignore datagrams from anyone else */
            s->len = 0;
            ok = udp_wait(s, deadline);
        }
        if (ok) {
            size_t n = MIN((size_t)s->len, *rn);
            memcpy(resp, s->buf, n);
            *rn = n;
        }
    }
    udp_unbind(s);
    kfree(s);
    return ok;
}

/* ------------------------------------------------------------------------
 * DHCP
 * ---------------------------------------------------------------------- */

struct PACKED dhcp {
    uint8_t op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t chaddr[16];
    uint8_t sname[64], file[128];
    uint32_t magic;
    uint8_t opts[312];
};

static size_t dhcp_build(struct iface *ifc, struct dhcp *d, uint8_t type, uint32_t xid, uint32_t req_ip, uint32_t server)
{
    memset(d, 0, sizeof(*d));
    d->op = 1;
    d->htype = 1;
    d->hlen = 6;
    d->xid = htonl(xid);
    d->flags = htons(0x8000);
    memcpy(d->chaddr, ifc->nd->mac, 6);
    d->magic = htonl(0x63825363);
    uint8_t *o = d->opts;
    *o++ = 53; *o++ = 1; *o++ = type;
    if (req_ip) { *o++ = 50; *o++ = 4; uint32_t v = htonl(req_ip); memcpy(o, &v, 4); o += 4; }
    if (server) { *o++ = 54; *o++ = 4; uint32_t v = htonl(server); memcpy(o, &v, 4); o += 4; }
    *o++ = 61; *o++ = 7; *o++ = 1; memcpy(o, ifc->nd->mac, 6); o += 6;         /* client id */
    {
        const char *hn = sys_hostname();
        size_t hl = MIN(strlen(hn), (size_t)63);
        *o++ = 12; *o++ = (uint8_t)hl; memcpy(o, hn, hl); o += hl;
    }
    *o++ = 55; *o++ = 3; *o++ = 1; *o++ = 3; *o++ = 6;
    *o++ = 255;
    return (size_t)(o - (uint8_t *)d);
}

static void dhcp_parse(struct dhcp *d, int len, uint8_t *type, uint32_t *mask, uint32_t *router, uint32_t *dns, uint32_t *server)
{
    uint8_t *o = d->opts, *end = (uint8_t *)d + len;
    while (o + 2 <= end && *o != 255) {
        if (*o == 0) { o++; continue; }
        uint8_t code = o[0], l = o[1];
        uint8_t *v = o + 2;
        if (v + l > end) break;
        uint32_t x = 0;
        if (l >= 4) { memcpy(&x, v, 4); x = ntohl(x); }
        switch (code) {
        case 53: if (l) *type = v[0]; break;
        case 1: *mask = x; break;
        case 3: *router = x; break;
        case 6: *dns = x; break;
        case 54: *server = x; break;
        }
        o += 2 + l;
    }
}

static bool dhcp_iface(struct iface *ifc, int timeout_ms)
{
    struct udp_sock *s = kzalloc(sizeof(*s));
    struct dhcp *d = kmalloc(sizeof(*d));
    if (!s || !d || !udp_bind(s, 68, ifc)) { kfree(s); kfree(d); return false; }
    uint32_t xid;
    random_bytes(&xid, sizeof(xid));
    uint32_t saved_ip = ifc->info.ip;
    ifc->info.ip = 0;
    bool ok = false;
    uint64_t deadline = uptime_ms() + (uint64_t)timeout_ms;
    size_t n = dhcp_build(ifc, d, 1, xid, 0, 0);                            /* DISCOVER */
    udp_broadcast(ifc, 68, 67, d, n);
    uint32_t offer = 0, server = 0, mask = 0, router = 0, dns = 0;
    while (udp_wait(s, deadline)) {
        struct dhcp *r = (struct dhcp *)s->buf;
        uint8_t type = 0;
        if (s->len >= 240 && ntohl(r->xid) == xid && !memcmp(r->chaddr, ifc->nd->mac, 6)) {
            dhcp_parse(r, s->len, &type, &mask, &router, &dns, &server);
            if (type == 2 && !offer) {                                       /* OFFER */
                offer = ntohl(r->yiaddr);
                n = dhcp_build(ifc, d, 3, xid, offer, server);               /* REQUEST */
                s->len = 0;
                udp_broadcast(ifc, 68, 67, d, n);
                continue;
            }
            if (type == 5 && offer) {                                        /* ACK */
                ifc->info.netmask = mask ? mask : 0xFFFFFF00;
                ifc->info.gateway = router;
                ifc->info.dns = dns ? dns : router;
                ifc->info.ip = ntohl(r->yiaddr);
                ok = true;
                break;
            }
            if (type == 6) break;                                            /* NAK */
        }
        s->len = 0;
    }
    udp_unbind(s);
    kfree(s);
    kfree(d);
    if (!ok) ifc->info.ip = saved_ip;
    else {
        char ip[20], gw[20];
        ip_to_str(ifc->info.ip, ip);
        ip_to_str(ifc->info.gateway, gw);
        klog("net: %s: DHCP lease %s, gateway %s", ifc->info.ifname, ip, gw);
    }
    return ok;
}

bool net_dhcp(int timeout_ms)
{
    bool any = false;
    for (int i = 0; i < nifs; i++) {
        struct iface *ifc = &ifs[i];
        if (ifc->nd->gone || !ifc->info.link) continue;
        if (dhcp_iface(ifc, timeout_ms)) { any = true; ifc->dhcp_fails = 0; }
    }
    return any;
}

/* ------------------------------------------------------------------------
 * DNS
 * ---------------------------------------------------------------------- */

bool net_resolve(const char *host, uint32_t *ip, int timeout_ms)
{
    if (str_to_ip(host, ip)) return true;
    if (!strcmp(host, "localhost")) { *ip = 0x7F000001; return true; }
    struct iface *ifc = default_iface();
    if (!ifc || !ifc->info.dns) return false;
    uint32_t dns_server = ifc->info.dns;
    uint8_t q[512];
    memset(q, 0, 12);
    uint16_t id = (uint16_t)rdtsc();
    q[0] = (uint8_t)(id >> 8); q[1] = (uint8_t)id;
    q[2] = 0x01;                /* recursion desired */
    q[5] = 1;                   /* one question */
    size_t p = 12;
    const char *s = host;
    while (*s && p < 400) {
        const char *dot = strchr(s, '.');
        size_t l = dot ? (size_t)(dot - s) : strlen(s);
        q[p++] = (uint8_t)l;
        memcpy(q + p, s, l);
        p += l;
        s += l;
        if (*s == '.') s++;
    }
    q[p++] = 0;
    q[p++] = 0; q[p++] = 1;     /* type A */
    q[p++] = 0; q[p++] = 1;     /* class IN */
    static struct udp_sock sock;
    uint16_t port = (uint16_t)(40000 + (rdtsc() & 0x3FFF));
    udp_bind(&sock, port, NULL);
    bool ok = false;
    for (int attempt = 0; attempt < 2 && !ok; attempt++) {
        udp_send(dns_server, port, 53, q, p);
        uint64_t deadline = uptime_ms() + (uint64_t)timeout_ms / 2;
        while (udp_wait(&sock, deadline)) {
            uint8_t *r = sock.buf;
            int len = sock.len;
            if (len > 12 && r[0] == q[0] && r[1] == q[1]) {
                int an = r[6] << 8 | r[7];
                int off = 12;
                /* skip the question */
                while (off < len && r[off]) off += r[off] + 1;
                off += 5;
                for (int i = 0; i < an && off + 12 <= len; i++) {
                    if ((r[off] & 0xC0) == 0xC0) off += 2;
                    else { while (off < len && r[off]) off += r[off] + 1; off++; }
                    int type = r[off] << 8 | r[off + 1];
                    int rdlen = r[off + 8] << 8 | r[off + 9];
                    off += 10;
                    if (type == 1 && rdlen == 4 && off + 4 <= len) {
                        *ip = (uint32_t)r[off] << 24 | (uint32_t)r[off + 1] << 16 | (uint32_t)r[off + 2] << 8 | r[off + 3];
                        ok = true;
                        break;
                    }
                    off += rdlen;
                }
                break;
            }
            sock.len = 0;
        }
        sock.len = 0;
    }
    udp_unbind(&sock);
    return ok;
}

/* ------------------------------------------------------------------------
 * receive path
 * ---------------------------------------------------------------------- */

static bool l4_csum_ok(uint32_t src, uint32_t dst, uint8_t proto, const uint8_t *seg, size_t len)
{
    uint32_t sum = (src >> 16) + (src & 0xFFFF) + (dst >> 16) + (dst & 0xFFFF) + proto + (uint32_t)len;
    return net_csum(seg, len, sum) == 0;
}

static void handle_frame(struct iface *ifc, uint8_t *f, size_t len)
{
    if (len < sizeof(struct eth)) return;
    struct eth *e = (struct eth *)f;
    /* adapters without a working address filter hand us other hosts' frames too */
    if (!(e->dst[0] & 1) && memcmp(e->dst, ifc->nd->mac, 6)) return;
    uint16_t type = ntohs(e->type);
    uint8_t *p = f + sizeof(*e);
    size_t pl = len - sizeof(*e);
    if (type == ETH_ARP && pl >= sizeof(struct arp)) {
        struct arp *a = (struct arp *)p;
        uint32_t spa = ntohl(a->spa), tpa = ntohl(a->tpa);
        if (spa) arp_store(ifc, spa, a->sha);
        if (ntohs(a->op) == 1 && ifc->info.ip && tpa == ifc->info.ip) arp_send(ifc, 2, a->sha, spa);
        return;
    }
    if (type != ETH_IP || pl < sizeof(struct ip4)) return;
    struct ip4 *h = (struct ip4 *)p;
    size_t hl = (size_t)(h->vihl & 15) * 4;
    size_t tl = ntohs(h->len);
    if ((h->vihl >> 4) != 4 || hl < 20 || hl > tl || tl > pl || net_csum(h, hl, 0) != 0) {
        ifc->info.rx_dropped++;
        return;
    }
    if (ntohs(h->frag) & 0x3FFF) return;                /* fragments: not supported */
    uint32_t dst = ntohl(h->dst), src = ntohl(h->src);
    uint32_t me = ifc->info.ip;
    if (me && dst != me && dst != 0xFFFFFFFF && (dst | ifc->info.netmask) != 0xFFFFFFFF) return;
    uint8_t *payload = p + hl;
    size_t plen = tl - hl;
    switch (h->proto) {
    case 1:
        if (net_csum(payload, plen, 0) != 0) { ifc->info.rx_dropped++; return; }
        icmp_input(src, payload, plen);
        break;
    case 17:
        if (plen >= sizeof(struct udp) && ((struct udp *)payload)->csum && !l4_csum_ok(src, dst, 17, payload, plen)) {
            ifc->info.rx_dropped++;
            return;
        }
        udp_input(ifc, src, payload, plen);
        break;
    case 6:
        if (!l4_csum_ok(src, dst, 6, payload, plen)) { ifc->info.rx_dropped++; return; }
        tcp_input(src, payload, plen);
        break;
    }
}

static void link_changed(struct iface *ifc, bool up)
{
    ifc->info.link = up;
    if (up) {
        ifc->dhcp_next = uptime_ms();
        ifc->dhcp_fails = 0;
        sched_wake(&netcfg_chan);
    } else {
        /* the lease is only good on the network we are no longer on */
        ifc->info.ip = 0;
        spin_lock(&net_lock);
        memset(ifc->arp, 0, sizeof(ifc->arp));
        spin_unlock(&net_lock);
    }
    klog("net: %s: link %s", ifc->info.ifname, up ? "up" : "down");
}

static int net_thread(void *arg)
{
    UNUSED(arg);
    static uint8_t frame[2048];
    for (;;) {
        uint64_t now = uptime_ms();
        for (int i = 0; i < nifs; i++) {
            struct iface *ifc = &ifs[i];
            struct netdev *nd = ifc->nd;
            if (nd->gone) {
                if (ifc->info.link) link_changed(ifc, false);
                continue;
            }
            int n, budget = 256;
            while (budget-- > 0 && (n = nd->poll(nd, frame, sizeof(frame))) > 0) {
                ifc->info.rx_packets++;
                ifc->info.rx_bytes += (uint64_t)n;
                handle_frame(ifc, frame, (size_t)n);
                last_rx_t = now;
            }
            if (now - ifc->link_checked >= 500) {
                ifc->link_checked = now;
                bool up = nd->link(nd);
                if (up != ifc->info.link) link_changed(ifc, up);
            }
        }
        tcp_timer();
        /* poll fast while traffic is flowing, relax when idle */
        sched_sleep(uptime_ms() - last_rx_t < 2000 ? 1 : 10);
    }
    return 0;
}

/* keeps every interface with a link configured */
static int netcfg_thread(void *arg)
{
    UNUSED(arg);
    sched_sleep(300);
    for (;;) {
        uint64_t now = uptime_ms();
        for (int i = 0; i < nifs; i++) {
            struct iface *ifc = &ifs[i];
            if (ifc->nd->gone || !ifc->info.link || ifc->info.ip || now < ifc->dhcp_next) continue;
            if (dhcp_iface(ifc, 3000)) {
                ifc->dhcp_fails = 0;
            } else {
                ifc->dhcp_fails++;
                ifc->dhcp_next = uptime_ms() + (uint64_t)MIN(ifc->dhcp_fails * 5000, 30000);
            }
        }
        sched_wait(&netcfg_chan, NULL, 250);
    }
    return 0;
}

static void fill_info(struct iface *ifc, struct net_info *out)
{
    *out = ifc->info;
    out->is_default = ifc == default_iface();
    if (ifc->nd->gone) out->link = false;
}

bool net_get_info(struct net_info *out)
{
    if (!nifs) return false;
    struct iface *ifc = default_iface();
    if (!ifc) {
        /* nothing configured: report the first adapter that is still there */
        ifc = &ifs[0];
        for (int i = 0; i < nifs; i++)
            if (!ifs[i].nd->gone) { ifc = &ifs[i]; break; }
    }
    fill_info(ifc, out);
    return true;
}

int net_iface_count(void) { return nifs; }

bool net_iface_get(int i, struct net_info *out)
{
    if (i < 0 || i >= nifs) return false;
    fill_info(&ifs[i], out);
    return true;
}

void netdev_register(struct netdev *d)
{
    spin_lock(&if_lock);
    bool usb = d->driver && (!strcmp(d->driver, "cdc_ecm") || !strcmp(d->driver, "rndis"));
    const char *prefix = usb ? "usb" : "eth";
    struct iface *ifc = NULL;
    /* a replugged USB adapter gets its old name back */
    for (int i = 0; i < nifs && !ifc; i++)
        if (ifs[i].nd->gone && !memcmp(ifs[i].info.mac, d->mac, 6)) ifc = &ifs[i];
    for (int i = 0; i < nifs && !ifc; i++)
        if (ifs[i].nd->gone && !strncmp(ifs[i].info.ifname, prefix, 3)) ifc = &ifs[i];
    if (!ifc && nifs < MAX_IF) {
        ifc = &ifs[nifs];
        int count = 0;
        for (int i = 0; i < nifs; i++) if (!strncmp(ifs[i].info.ifname, prefix, 3)) count++;
        memset(ifc, 0, sizeof(*ifc));
        snprintf(ifc->info.ifname, sizeof(ifc->info.ifname), "%s%d", prefix, count);
        ifc->nd = d;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        nifs++;
    } else if (ifc) {
        char name[8];
        strlcpy(name, ifc->info.ifname, sizeof(name));
        memset(&ifc->info, 0, sizeof(ifc->info));
        memset(ifc->arp, 0, sizeof(ifc->arp));
        strlcpy(ifc->info.ifname, name, sizeof(ifc->info.ifname));
        ifc->nd = d;
    }
    if (ifc) {
        strlcpy(ifc->info.driver, d->name, sizeof(ifc->info.driver));
        strlcpy(ifc->info.drv, d->driver ? d->driver : "", sizeof(ifc->info.drv));
        memcpy(ifc->info.mac, d->mac, 6);
        ifc->info.link = false;             /* the network thread notices the link and starts DHCP */
        ifc->link_checked = 0;
        ifc->dhcp_next = 0;
    } else {
        klog("net: too many network adapters, ignoring %s", d->name);
    }
    spin_unlock(&if_lock);
}

void net_init(void)
{
    net_probe_pci();
    if (!nifs) klog("net: no supported network adapter yet (USB adapters are picked up when plugged in)");
    thread_create_ex("net", net_thread, NULL, 1, -1, NULL, 32 * 1024);
    thread_create("netcfg", netcfg_thread, NULL);
    ntp_init();
}

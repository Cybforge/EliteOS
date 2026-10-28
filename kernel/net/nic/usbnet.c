/*
 * USB network adapters: CDC Ethernet (ECM) and Microsoft RNDIS, which is
 * what Android phones speak for "USB tethering" - the phone shares its
 * Wi-Fi or mobile connection over the cable and ZenithOS sees a wired
 * adapter (usb0) that gets its address by DHCP.
 *
 * Received frames are queued by the USB thread and picked up by the
 * network thread; transmit goes straight to the bulk OUT endpoint.
 */
#include <kernel.h>
#include <dev.h>
#include <mm.h>
#include <spinlock.h>
#include <usb.h>
#include <x86.h>
#include "../netdev.h"

#define NRXB 16             /* bulk IN transfers in flight */
#define RXB_SIZE 16384      /* RNDIS may pack several frames into one transfer */
#define NTXB 32
#define TXB_SIZE 2048
#define RXQ 128
#define FRAME_MAX 1536
#define TAG_ZLP 0xFFFF

#define RNDIS_PACKET_MSG      0x00000001
#define RNDIS_INITIALIZE_MSG  0x00000002
#define RNDIS_QUERY_MSG       0x00000004
#define RNDIS_SET_MSG         0x00000005
#define RNDIS_INDICATE_STATUS 0x00000007
#define RNDIS_KEEPALIVE_MSG   0x00000008
#define RNDIS_KEEPALIVE_CMPLT 0x80000008
#define OID_GEN_MEDIA_CONNECT_STATUS  0x00010114
#define OID_GEN_CURRENT_PACKET_FILTER 0x0001010E
#define OID_802_3_PERMANENT_ADDRESS   0x01010101
#define STATUS_MEDIA_CONNECT    0x4001000B
#define STATUS_MEDIA_DISCONNECT 0x4001000C

struct unet {
    struct netdev nd;
    struct usb_dev *ud;
    bool rndis;
    int ctrl_iface;
    struct usb_ep *in, *out, *intr;
    int mps_out;
    uint8_t *rxbuf, *txbuf, *intbuf;
    uint64_t rxbuf_phys, txbuf_phys, intbuf_phys;
    volatile bool tx_busy[NTXB];
    unsigned tx_next;
    spinlock_t txlock;
    uint8_t (*q)[FRAME_MAX];
    uint16_t qlen[RXQ];
    unsigned qhead, qtail;
    spinlock_t qlock;
    volatile bool link, pending;
    uint32_t reqid;
    struct unet *next;
};

static struct unet *all;
static spinlock_t all_lock = SPINLOCK_INIT("usbnet");

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* ------------------------------------------------------------------------
 * data path
 * ---------------------------------------------------------------------- */

static void enqueue(struct unet *u, const uint8_t *f, size_t len)
{
    if (len < 14 || len > FRAME_MAX) return;
    spin_lock(&u->qlock);
    unsigned next = (u->qtail + 1) % RXQ;
    if (next != u->qhead) {
        memcpy(u->q[u->qtail], f, len);
        u->qlen[u->qtail] = (uint16_t)len;
        u->qtail = next;
    }
    spin_unlock(&u->qlock);
}

static void rx_done(struct unet *u, uintptr_t tag, uint32_t actual)
{
    uint8_t *b = u->rxbuf + (size_t)tag * RXB_SIZE;
    if (!u->rndis) {
        enqueue(u, b, actual);
    } else {
        for (uint32_t off = 0; off + 44 <= actual;) {
            uint32_t type = le32(b + off), len = le32(b + off + 4);
            if (len < 8 || off + len > actual) break;
            if (type == RNDIS_PACKET_MSG) {
                uint32_t doff = le32(b + off + 8), dlen = le32(b + off + 12);
                if (8 + doff + dlen <= len) enqueue(u, b + off + 8 + doff, dlen);
            }
            off += len;
        }
    }
    usb_submit(u->in, u->rxbuf_phys + (uint64_t)tag * RXB_SIZE, RXB_SIZE, tag);
}

static void intr_done(struct unet *u, uint32_t actual)
{
    const uint8_t *n = u->intbuf;
    if (!u->rndis && actual >= 8 && n[0] == 0xA1 && n[1] == 0x00) {
        bool up = n[2] | n[3];                              /* NETWORK_CONNECTION */
        if (up != u->link) klog("usbnet: %s", up ? "cable connected" : "cable disconnected");
        u->link = up;
    } else if (u->rndis && actual >= 1) {
        u->pending = true;                                  /* RESPONSE_AVAILABLE */
    }
    usb_submit(u->intr, u->intbuf_phys, 64, 0);
}

static void done(void *ctx, struct usb_ep *ep, uintptr_t tag, int cc, uint32_t actual)
{
    struct unet *u = ctx;
    if (cc != 1 && cc != 13) actual = 0;
    if (ep == u->in) rx_done(u, tag, actual);
    else if (ep == u->intr) intr_done(u, actual);
    else if (ep == u->out && tag != TAG_ZLP && tag < NTXB) u->tx_busy[tag] = false;
}

static bool un_send(struct netdev *nd, const void *data, size_t len)
{
    struct unet *u = nd->priv;
    size_t hdr = u->rndis ? 44 : 0;
    if (len + hdr > TXB_SIZE || nd->gone) return false;
    spin_lock(&u->txlock);
    unsigned i = u->tx_next;
    for (int k = 0; k < 200000 && u->tx_busy[i]; k++) cpu_relax();
    if (u->tx_busy[i]) { spin_unlock(&u->txlock); return false; }
    uint8_t *b = u->txbuf + (size_t)i * TXB_SIZE;
    if (u->rndis) {
        memset(b, 0, 44);
        put32(b, RNDIS_PACKET_MSG);
        put32(b + 4, (uint32_t)(44 + len));
        put32(b + 8, 36);                                   /* data offset from byte 8 */
        put32(b + 12, (uint32_t)len);
    }
    memcpy(b + hdr, data, len);
    size_t total = hdr + len;
    uint64_t phys = u->txbuf_phys + (uint64_t)i * TXB_SIZE;
    /* full packets first, then the short tail (or a zero-length packet) that ends the transfer */
    size_t tail = total % (size_t)u->mps_out;
    u->tx_busy[i] = true;
    u->tx_next = (i + 1) % NTXB;
    bool ok;
    if (total - tail) {
        ok = usb_submit(u->out, phys, (uint32_t)(total - tail), tail ? TAG_ZLP : i);
        if (ok) ok = usb_submit(u->out, phys + total - tail, (uint32_t)tail, tail ? i : TAG_ZLP);
    } else {
        ok = usb_submit(u->out, phys, (uint32_t)tail, i);
    }
    if (!ok) u->tx_busy[i] = false;
    spin_unlock(&u->txlock);
    return ok;
}

static int un_poll(struct netdev *nd, void *buf, size_t cap)
{
    struct unet *u = nd->priv;
    int got = 0;
    spin_lock(&u->qlock);
    if (u->qhead != u->qtail) {
        got = (int)MIN((size_t)u->qlen[u->qhead], cap);
        memcpy(buf, u->q[u->qhead], (size_t)got);
        u->qhead = (u->qhead + 1) % RXQ;
    }
    spin_unlock(&u->qlock);
    return got;
}

static bool un_link(struct netdev *nd)
{
    struct unet *u = nd->priv;
    return u->link && !nd->gone;
}

static void un_detach(void *ctx)
{
    struct unet *u = ctx;
    u->nd.gone = true;
    u->link = false;
}

/* ------------------------------------------------------------------------
 * RNDIS control messages
 * ---------------------------------------------------------------------- */

static uint8_t rbuf[1024];

static int rndis_cmd(struct unet *u, uint8_t *msg, uint32_t len, uint32_t want)
{
    if (usb_ctrl(u->ud, 0x21, 0x00, 0, (uint16_t)u->ctrl_iface, msg, (uint16_t)len)) return -1;
    for (int tries = 0; tries < 50; tries++) {
        memset(rbuf, 0, 16);
        if (!usb_ctrl(u->ud, 0xA1, 0x01, 0, (uint16_t)u->ctrl_iface, rbuf, sizeof(rbuf))) {
            uint32_t type = le32(rbuf);
            if (type == want && le32(rbuf + 8) == le32(msg + 8)) return (int)le32(rbuf + 12);   /* status */
            if (type == RNDIS_INDICATE_STATUS) {
                uint32_t st = le32(rbuf + 8);
                if (st == STATUS_MEDIA_CONNECT) u->link = true;
                if (st == STATUS_MEDIA_DISCONNECT) u->link = false;
            }
        }
        mdelay(10);
    }
    return -1;
}

static int rndis_query(struct unet *u, uint32_t oid, uint8_t *out, uint32_t outlen)
{
    /* like other hosts, send an (empty) 48-byte information buffer along; some devices insist */
    uint8_t m[28 + 48];
    memset(m, 0, sizeof(m));
    put32(m, RNDIS_QUERY_MSG);
    put32(m + 4, sizeof(m));
    put32(m + 8, ++u->reqid);
    put32(m + 12, oid);
    put32(m + 16, 48);
    put32(m + 20, 20);
    if (rndis_cmd(u, m, sizeof(m), 0x80000000u | RNDIS_QUERY_MSG) != 0) return -1;
    uint32_t n = le32(rbuf + 16), off = le32(rbuf + 20);
    if (8 + off + n > sizeof(rbuf)) return -1;
    memcpy(out, rbuf + 8 + off, MIN(n, outlen));
    return (int)n;
}

static int rndis_set(struct unet *u, uint32_t oid, uint32_t value)
{
    uint8_t m[32];
    memset(m, 0, sizeof(m));
    put32(m, RNDIS_SET_MSG);
    put32(m + 4, 32);
    put32(m + 8, ++u->reqid);
    put32(m + 12, oid);
    put32(m + 16, 4);
    put32(m + 20, 20);
    put32(m + 28, value);
    return rndis_cmd(u, m, 32, 0x80000000u | RNDIS_SET_MSG);
}

static bool rndis_init(struct unet *u)
{
    uint8_t m[24];
    put32(m, RNDIS_INITIALIZE_MSG);
    put32(m + 4, 24);
    put32(m + 8, ++u->reqid);
    put32(m + 12, 1);
    put32(m + 16, 0);
    put32(m + 20, RXB_SIZE);
    if (rndis_cmd(u, m, 24, 0x80000000u | RNDIS_INITIALIZE_MSG) != 0) return false;
    if (rndis_query(u, OID_802_3_PERMANENT_ADDRESS, u->nd.mac, 6) != 6) return false;
    uint8_t st[4] = { 0 };
    if (rndis_query(u, OID_GEN_MEDIA_CONNECT_STATUS, st, 4) == 4) u->link = le32(st) == 0;
    else u->link = true;
    /* directed, all multicast, broadcast: the device starts passing data */
    return rndis_set(u, OID_GEN_CURRENT_PACKET_FILTER, 0x1 | 0x4 | 0x8) == 0;
}

/* status indications arrive as "response available" on the interrupt endpoint */
void usbnet_service(void)
{
    for (struct unet *u = all; u; u = u->next) {
        if (!u->pending || u->nd.gone) continue;
        u->pending = false;
        if (usb_ctrl(u->ud, 0xA1, 0x01, 0, (uint16_t)u->ctrl_iface, rbuf, sizeof(rbuf))) continue;
        uint32_t type = le32(rbuf);
        if (type == RNDIS_INDICATE_STATUS) {
            uint32_t st = le32(rbuf + 8);
            if (st == STATUS_MEDIA_CONNECT || st == STATUS_MEDIA_DISCONNECT) {
                u->link = st == STATUS_MEDIA_CONNECT;
                klog("usbnet: %s", u->link ? "media connected" : "media disconnected");
            }
        } else if (type == RNDIS_KEEPALIVE_MSG) {
            uint8_t m[16];
            put32(m, RNDIS_KEEPALIVE_CMPLT);
            put32(m + 4, 16);
            memcpy(m + 8, rbuf + 8, 4);
            put32(m + 12, 0);
            usb_ctrl(u->ud, 0x21, 0x00, 0, (uint16_t)u->ctrl_iface, m, 16);
        }
    }
}

/* ------------------------------------------------------------------------
 * probing
 * ---------------------------------------------------------------------- */

struct found {
    bool ecm, rndis;
    int cfg_value;
    int ctrl_iface, data_iface, data_alt;
    int imac;
    struct usb_ep_desc in, out, intr;
    bool have_intr;
};

static int hexval(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }

/* walk one configuration descriptor looking for an ECM or RNDIS function */
static void parse_config(const uint8_t *c, int total, struct found *f)
{
    memset(f, 0, sizeof(*f));
    f->cfg_value = c[5];
    f->data_iface = -1;
    int iface = -1, alt = 0, cls = 0, sub = 0, proto = 0;
    struct usb_ep_desc in = { 0 }, out = { 0 };
    for (int off = 0; off + 2 <= total && c[off] >= 2; off += c[off]) {
        const uint8_t *d = c + off;
        if (d[1] == 4 && d[0] >= 9) {
            if (f->data_iface == iface && in.addr && out.addr && (f->ecm || f->rndis) && !f->in.addr) {
                f->in = in;
                f->out = out;
                f->data_alt = alt;
            }
            iface = d[2];
            alt = d[3];
            cls = d[5];
            sub = d[6];
            proto = d[7];
            in.addr = out.addr = 0;
            if (cls == 2 && sub == 6 && !f->rndis) { f->ecm = true; f->ctrl_iface = iface; f->data_iface = iface + 1; }
            if (((cls == 0xE0 && sub == 1 && proto == 3) || (cls == 2 && sub == 2 && proto == 0xFF)) && !f->ecm) {
                f->rndis = true;
                f->ctrl_iface = iface;
                f->data_iface = iface + 1;
            }
        } else if (d[1] == 0x24 && d[0] >= 5 && d[2] == 0x06) {
            if (f->ecm || f->rndis) f->data_iface = d[4];                   /* union: first subordinate */
        } else if (d[1] == 0x24 && d[0] >= 13 && d[2] == 0x0F) {
            f->imac = d[3];                                                /* Ethernet networking */
        } else if (d[1] == 5 && d[0] >= 7) {
            struct usb_ep_desc e = { d[2], d[3], (uint16_t)(d[4] | d[5] << 8), d[6] };
            if ((f->ecm || f->rndis) && iface == f->ctrl_iface && (e.attr & 3) == 3 && (e.addr & 0x80)) {
                f->intr = e;
                f->have_intr = true;
            }
            if (iface == f->data_iface && (e.attr & 3) == 2) {
                if (e.addr & 0x80) in = e;
                else out = e;
            }
        }
    }
    if (f->data_iface == iface && in.addr && out.addr && !f->in.addr) {
        f->in = in;
        f->out = out;
        f->data_alt = alt;
    }
    if (!f->in.addr) f->ecm = f->rndis = false;
}

bool usbnet_probe(struct usb_dev *d, const uint8_t *dev_desc)
{
    int ncfg = dev_desc[17];
    struct found best = { 0 }, f;
    static uint8_t cfg[1024];
    for (int i = 0; i < ncfg && i < 4; i++) {
        if (usb_ctrl(d, 0x80, 6, (uint16_t)(0x0200 | i), 0, cfg, 9)) continue;
        int total = MIN(cfg[2] | cfg[3] << 8, (int)sizeof(cfg));
        if (usb_ctrl(d, 0x80, 6, (uint16_t)(0x0200 | i), 0, cfg, (uint16_t)total)) continue;
        parse_config(cfg, total, &f);
        /* CDC Ethernet is the simpler protocol: take it when a device offers both */
        if (f.ecm && !best.ecm) best = f;
        else if (f.rndis && !best.ecm && !best.rndis) best = f;
    }
    if (!best.ecm && !best.rndis) return false;

    struct unet *u = kzalloc(sizeof(*u));
    u->ud = d;
    u->rndis = best.rndis;
    u->ctrl_iface = best.ctrl_iface;
    u->txlock = (spinlock_t)SPINLOCK_INIT("usbnet-tx");
    u->qlock = (spinlock_t)SPINLOCK_INIT("usbnet-rx");
    u->mps_out = best.out.mps ? best.out.mps & 0x7FF : 64;
    u->rxbuf = dma_alloc(NRXB * RXB_SIZE, &u->rxbuf_phys);
    u->txbuf = dma_alloc(NTXB * TXB_SIZE, &u->txbuf_phys);
    u->intbuf = dma_alloc(64, &u->intbuf_phys);
    u->q = kmalloc(RXQ * FRAME_MAX);
    if (!u->rxbuf || !u->txbuf || !u->intbuf || !u->q) return false;

    if (usb_ctrl(d, 0x00, 9, (uint16_t)best.cfg_value, 0, NULL, 0)) return false;       /* SET_CONFIGURATION */
    if (best.data_alt) usb_ctrl(d, 0x01, 11, (uint16_t)best.data_alt, (uint16_t)best.data_iface, NULL, 0);

    if (u->rndis) {
        if (!rndis_init(u)) {
            klog("usbnet: RNDIS initialization failed");
            return false;
        }
    } else {
        /* the MAC address is a string descriptor of 12 hex digits */
        uint8_t s[64];
        bool ok = best.imac && !usb_ctrl(d, 0x80, 6, (uint16_t)(0x0300 | best.imac), 0x0409, s, sizeof(s)) && s[0] >= 26;
        for (int i = 0; ok && i < 6; i++) {
            int hi = hexval(s[2 + i * 4]), lo = hexval(s[4 + i * 4]);
            if (hi < 0 || lo < 0) ok = false;
            else u->nd.mac[i] = (uint8_t)(hi << 4 | lo);
        }
        if (!ok) {
            u->nd.mac[0] = 0x02;
            for (int i = 1; i < 6; i++) u->nd.mac[i] = (uint8_t)(rdtsc() >> (i * 7));
        }
        usb_ctrl(d, 0x21, 0x43, 0x0E, (uint16_t)u->ctrl_iface, NULL, 0);   /* packet filter: directed, broadcast, all multicast */
        u->link = true;
    }

    struct usb_ep_desc eps[3] = { best.in, best.out, best.intr };
    struct usb_ep *opened[3] = { 0 };
    if (!usb_open_endpoints(d, eps, best.have_intr ? 3 : 2, opened, done, u)) return false;
    u->in = opened[0];
    u->out = opened[1];
    u->intr = opened[2];
    for (int i = 0; i < NRXB; i++) usb_submit(u->in, u->rxbuf_phys + (uint64_t)i * RXB_SIZE, RXB_SIZE, (uintptr_t)i);
    if (u->intr) usb_submit(u->intr, u->intbuf_phys, 64, 0);

    uint16_t vid = (uint16_t)(dev_desc[8] | dev_desc[9] << 8), pid = (uint16_t)(dev_desc[10] | dev_desc[11] << 8);
    snprintf(u->nd.name, sizeof(u->nd.name), "USB %s adapter %04x:%04x", u->rndis ? "RNDIS" : "CDC Ethernet", vid, pid);
    usb_set_name(d, u->nd.name);
    usb_set_detach(d, un_detach, u);
    u->nd.driver = u->rndis ? "rndis" : "cdc_ecm";
    u->nd.send = un_send;
    u->nd.poll = un_poll;
    u->nd.link = un_link;
    u->nd.priv = u;
    spin_lock(&all_lock);
    u->next = all;
    all = u;
    spin_unlock(&all_lock);
    netdev_register(&u->nd);
    netdev_log(&u->nd, "usb");
    return true;
}

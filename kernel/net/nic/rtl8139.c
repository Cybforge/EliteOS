/*
 * Realtek RTL8139 family (and the many boards built on it): one receive
 * ring buffer the chip writes frames into back to back, four transmit
 * slots used round robin.
 */
#include <kernel.h>
#include <dev.h>
#include <mm.h>
#include <spinlock.h>
#include <x86.h>
#include "../netdev.h"

#define IDR0   0x00
#define TSD0   0x10
#define TSAD0  0x20
#define RBSTART 0x30
#define CR     0x37
#define CAPR   0x38
#define IMR    0x3C
#define ISR    0x3E
#define TCR    0x40
#define RCR    0x44
#define CFG9346 0x50
#define CONFIG1 0x52
#define BMCR   0x62
#define BMSR   0x64

#define CR_RST  0x10
#define CR_RE   0x08
#define CR_TE   0x04
#define CR_BUFE 0x01

#define RX_LEN  8192
#define RX_ALLOC (RX_LEN + 16 + 2048)   /* WRAP mode: the chip may write past the end */
#define TX_BUF 2048

struct rtl {
    struct netdev nd;
    uint16_t io;
    uint8_t *rx;
    uint8_t *tx[4];
    unsigned rx_off, tx_cur;
    spinlock_t tx_lock;
};

static bool rtl_send(struct netdev *nd, const void *data, size_t len)
{
    struct rtl *r = nd->priv;
    if (len > 1792) return false;
    spin_lock(&r->tx_lock);
    unsigned i = r->tx_cur;
    uint16_t tsd = (uint16_t)(r->io + TSD0 + i * 4);
    /* the slot is free once the chip has DMA'd it out (OWN set) */
    uint32_t st = inl(tsd);
    for (int k = 0; k < 100000 && !(st & (1u << 13)) && st; k++) { cpu_relax(); st = inl(tsd); }
    memcpy(r->tx[i], data, len);
    if (len < 60) { memset(r->tx[i] + len, 0, 60 - len); len = 60; }
    outl((uint16_t)(r->io + TSAD0 + i * 4), dma32(r->tx[i]));
    outl(tsd, (uint32_t)len | (8u << 16));          /* early tx threshold 256 bytes, clears OWN */
    r->tx_cur = (i + 1) & 3;
    spin_unlock(&r->tx_lock);
    return true;
}

static int rtl_poll(struct netdev *nd, void *buf, size_t cap)
{
    struct rtl *r = nd->priv;
    while (!(inb((uint16_t)(r->io + CR)) & CR_BUFE)) {
        uint8_t *p = r->rx + r->rx_off;
        uint16_t status = (uint16_t)(p[0] | p[1] << 8);
        uint16_t len = (uint16_t)(p[2] | p[3] << 8);          /* includes the CRC */
        if (len == 0xFFF0) return 0;                          /* the chip is still writing the header */
        int got = 0;
        if ((status & 1) && len >= 64 && len <= 1522) {
            got = MIN((int)len - 4, (int)cap);
            memcpy(buf, p + 4, (size_t)got);
        } else if (!(status & 1)) {
            /* a broken header: restart the receiver */
            outb((uint16_t)(r->io + CR), CR_TE);
            outb((uint16_t)(r->io + CR), CR_TE | CR_RE);
            outl((uint16_t)(r->io + RBSTART), dma32(r->rx));
            r->rx_off = 0;
            outw((uint16_t)(r->io + CAPR), (uint16_t)(0 - 16));
            return 0;
        }
        r->rx_off = (r->rx_off + len + 4 + 3) & ~3u;
        if (r->rx_off >= RX_LEN) r->rx_off -= RX_LEN;
        outw((uint16_t)(r->io + CAPR), (uint16_t)(r->rx_off - 16));
        outw((uint16_t)(r->io + ISR), 0x0001 | 0x0010);        /* ROK, RXOVW */
        if (got) return got;
    }
    return 0;
}

static bool rtl_link(struct netdev *nd)
{
    struct rtl *r = nd->priv;
    return inw((uint16_t)(r->io + BMSR)) & BMSR_LINK;
}

static struct netdev *rtl_attach(struct pci_dev *p, const struct nic_id *id)
{
    if (!pci_bar_is_io(p, 0)) return NULL;
    struct rtl *r = kzalloc(sizeof(*r));
    r->io = (uint16_t)pci_bar_addr(p, 0);
    r->tx_lock = (spinlock_t)SPINLOCK_INIT("rtl8139-tx");
    pci_enable_busmaster(p);

    outb((uint16_t)(r->io + CONFIG1), 0);                      /* wake up (LWAKE + LWPTN low) */
    outb((uint16_t)(r->io + CR), CR_RST);
    for (int i = 0; i < 1000 && (inb((uint16_t)(r->io + CR)) & CR_RST); i++) udelay(10);
    for (int i = 0; i < 6; i++) r->nd.mac[i] = inb((uint16_t)(r->io + IDR0 + i));

    r->rx = dma_alloc(RX_ALLOC, NULL);
    for (int i = 0; i < 4; i++) r->tx[i] = dma_alloc(TX_BUF, NULL);
    if (!r->rx || !r->tx[3]) return NULL;
    outl((uint16_t)(r->io + RBSTART), dma32(r->rx));
    outw((uint16_t)(r->io + IMR), 0);
    outw((uint16_t)(r->io + ISR), 0xFFFF);
    outb((uint16_t)(r->io + CR), CR_RE | CR_TE);
    /* accept broadcast, multicast, our address; WRAP; 8K ring; unlimited DMA burst; no rx threshold */
    outl((uint16_t)(r->io + RCR), 0x2 | 0x4 | 0x8 | (1u << 7) | (7u << 8) | (7u << 13));
    outl((uint16_t)(r->io + TCR), (6u << 8) | (3u << 24));        /* 1024-byte DMA bursts, standard IFG */
    outw((uint16_t)(r->io + CAPR), (uint16_t)(0 - 16));
    /* restart auto-negotiation */
    outw((uint16_t)(r->io + BMCR), inw((uint16_t)(r->io + BMCR)) | (1u << 12) | (1u << 9));

    snprintf(r->nd.name, sizeof(r->nd.name), "%s", id->model);
    r->nd.send = rtl_send;
    r->nd.poll = rtl_poll;
    r->nd.link = rtl_link;
    r->nd.priv = r;
    return &r->nd;
}

static const struct nic_id ids[] = {
    { 0x10EC, 0x8139, "Realtek RTL8139 Fast Ethernet" },
    { 0x10EC, 0x8138, "Realtek RTL8139 Fast Ethernet (CardBus)" },
    { 0x1113, 0x1211, "Accton / SMC 1211TX EZCard (RTL8139)" },
    { 0x1500, 0x1360, "Delta Electronics 8139 (RTL8139)" },
    { 0x4033, 0x1360, "Addtron 8139 (RTL8139)" },
    { 0x1186, 0x1300, "D-Link DFE-538TX (RTL8139)" },
    { 0x1186, 0x1340, "D-Link DFE-690TXD (RTL8139)" },
    { 0x13D1, 0xAB06, "AboCom FE2000VX (RTL8139)" },
    { 0x1259, 0xA117, "Allied Telesyn 8139 (RTL8139)" },
    { 0x14EA, 0xAB06, "Planex FNW-3603-TX (RTL8139)" },
    { 0x14EA, 0xAB07, "Planex FNW-3800-TX (RTL8139)" },
    { 0x1432, 0x9130, "Edimax EP-4103DL (RTL8139)" },
    { 0x018A, 0x0106, "LevelOne FPC-0106TX (RTL8139)" },
    { 0x126C, 0x1211, "Northern Telecom 8139 (RTL8139)" },
    { 0x021B, 0x8139, "Compaq HNE-300 (RTL8139)" },
    { 0 },
};

const struct nic_driver drv_rtl8139 = { "rtl8139", "Realtek RTL8139 Fast Ethernet", ids, rtl_attach };

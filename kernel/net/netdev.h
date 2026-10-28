#ifndef ZENITH_NETDEV_H
#define ZENITH_NETDEV_H

#include <kernel.h>
#include <dev.h>

/*
 * A network adapter as the stack sees it. Drivers fill one in and hand it
 * to netdev_register(); the network thread then polls it for frames.
 */
struct netdev {
    char name[48];              /* model, e.g. "Intel 82540EM Gigabit Ethernet" */
    const char *driver;         /* driver name, e.g. "e1000" */
    uint8_t mac[6];
    bool (*send)(struct netdev *d, const void *frame, size_t len);
    int  (*poll)(struct netdev *d, void *buf, size_t cap);      /* bytes, 0 if nothing */
    bool (*link)(struct netdev *d);
    void *priv;
    bool gone;                  /* unplugged (USB); the stack stops using it */
    uint32_t rx_window;         /* bytes the adapter can buffer; caps the TCP window (0 = no limit) */
};

void netdev_register(struct netdev *d);
void net_probe_pci(void);
void netdev_log(struct netdev *d, const char *bus);

/* PCI network drivers: an ID table and an attach function */
struct nic_id {
    uint16_t vendor, device;
    const char *model;
    uint32_t flags;             /* driver specific */
};

struct nic_driver {
    const char *name;
    const char *family;         /* shown by `netcards` */
    const struct nic_id *ids;
    struct netdev *(*attach)(struct pci_dev *p, const struct nic_id *id);
};

extern const struct nic_driver drv_e1000, drv_e1000e, drv_igb, drv_eepro100, drv_rtl8139, drv_r8169, drv_pcnet,
    drv_ne2k, drv_tulip, drv_virtio_net, drv_vmxnet3;

/* contiguous, zeroed DMA memory below 4 GiB (most NICs take 32-bit addresses) */
void *dma_alloc(size_t bytes, uint64_t *phys);
uint32_t dma32(const void *va);

/* 93Cxx serial EEPROM ("Microwire"), bit-banged through a chip register */
struct microwire {
    void *ctx;
    void (*out)(void *ctx, bool cs, bool sk, bool di);
    bool (*in)(void *ctx);
    int abits;                  /* address width, found by microwire_probe */
};
void     microwire_probe(struct microwire *m);
uint16_t microwire_read(struct microwire *m, int addr);

/* MII PHY registers */
#define MII_BMCR   0
#define MII_BMSR   1
#define BMSR_LINK  (1u << 2)

/* between the layers of the stack */
uint16_t net_csum(const void *data, size_t len, uint32_t sum);
bool     net_ip_send(uint32_t dst, uint8_t proto, const void *payload, size_t len);
uint32_t net_src_ip(uint32_t dst);
uint32_t net_rx_window(uint32_t dst);
void     tcp_input(uint32_t src, uint8_t *data, size_t len);
void     tcp_timer(void);

static inline uint32_t mmio_r32(volatile uint8_t *b, uint32_t r) { return *(volatile uint32_t *)(b + r); }
static inline uint16_t mmio_r16(volatile uint8_t *b, uint32_t r) { return *(volatile uint16_t *)(b + r); }
static inline uint8_t  mmio_r8(volatile uint8_t *b, uint32_t r) { return *(volatile uint8_t *)(b + r); }
static inline void mmio_w32(volatile uint8_t *b, uint32_t r, uint32_t v) { *(volatile uint32_t *)(b + r) = v; }
static inline void mmio_w16(volatile uint8_t *b, uint32_t r, uint16_t v) { *(volatile uint16_t *)(b + r) = v; }
static inline void mmio_w8(volatile uint8_t *b, uint32_t r, uint8_t v) { *(volatile uint8_t *)(b + r) = v; }

#endif

#pragma once
#include "kernel.h"
#include "dev.h"
#include "mm.h"
#include "spinlock.h"
struct netdev {
    char name[48]; const char *driver; uint8_t mac[6];
    bool (*send)(struct netdev *d, const void *frame, size_t len);
    int (*poll)(struct netdev *d, void *buf, size_t cap);
    bool (*link)(struct netdev *d);
    void *priv; bool gone; uint32_t rx_window;
};
struct nic_id { uint16_t vendor, device; const char *model; uint32_t flags; };
struct nic_driver { const char *name; const char *family; const struct nic_id *ids; struct netdev *(*attach)(struct pci_dev *p, const struct nic_id *id); };
void *dma_alloc(size_t bytes, uint64_t *phys);
#define MII_BMCR 0
#define MII_BMSR 1
#define BMSR_LINK (1u << 2)
/* every register access goes to the device model */
uint32_t model_read(uint32_t off, int size);
void model_write(uint32_t off, int size, uint32_t v);
static inline uint32_t mmio_r32(volatile uint8_t *b, uint32_t r) { (void)b; return model_read(r, 4); }
static inline uint16_t mmio_r16(volatile uint8_t *b, uint32_t r) { (void)b; return (uint16_t)model_read(r, 2); }
static inline uint8_t mmio_r8(volatile uint8_t *b, uint32_t r) { (void)b; return (uint8_t)model_read(r, 1); }
static inline void mmio_w32(volatile uint8_t *b, uint32_t r, uint32_t v) { (void)b; model_write(r, 4, v); }
static inline void mmio_w16(volatile uint8_t *b, uint32_t r, uint16_t v) { (void)b; model_write(r, 2, v); }
static inline void mmio_w8(volatile uint8_t *b, uint32_t r, uint8_t v) { (void)b; model_write(r, 1, v); }

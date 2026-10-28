#pragma once
#include "kernel.h"
struct pci_dev { uint8_t bus, dev, fn; uint16_t vendor, device; uint8_t cls, subcls, progif, rev, irq_line, irq_pin; uint32_t bar[6]; };
void udelay(uint64_t us);
void mdelay(uint64_t ms);
void pci_enable_busmaster(struct pci_dev *d);
uint64_t pci_bar_addr(struct pci_dev *d, int bar);
uint8_t pci_find_cap(struct pci_dev *d, uint8_t id, uint8_t after);
uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off);
void pci_write16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint16_t v);
static inline bool pci_bar_is_io(struct pci_dev *d, int bar) { return d->bar[bar] & 1; }

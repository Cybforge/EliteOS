#ifndef ZENITH_DEV_H
#define ZENITH_DEV_H

#include <kernel.h>
#include <bootinfo.h>

struct regs;

/* serial.c */
void serial_init(void);
void serial_write(const char *s, size_t n);
int  serial_read(void);

/* bootcon.c - framebuffer text console used during boot and for panics */
void bootcon_init(struct boot_framebuffer *fb);
void bootcon_write(const char *s, size_t n);
void bootcon_enable(bool on);
void bootcon_status(const char *msg, int pct);

/* panic.c */
NORETURN void panic_regs(struct regs *r, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
bool panic_in_progress(void);

/* acpi.c */
struct acpi_info {
    int      ncpus;
    uint32_t lapic_ids[64];
    uint64_t lapic_phys;
    uint64_t ioapic_phys;
    uint32_t ioapic_gsi_base;
    struct { uint8_t used; uint32_t gsi; uint16_t flags; } iso[16];  /* ISA IRQ overrides */
    uint32_t pm1a_cnt, pm1b_cnt;
    uint16_t slp_typa, slp_typb;
    bool     s5_valid;
    uint8_t  reset_space;
    uint64_t reset_addr;
    uint8_t  reset_value;
    bool     reset_valid;
    uint16_t century_reg;
    uint16_t pm_tmr_port;
    bool     pm_tmr_32;
    char     oem[7];
    uint64_t hpet_phys;
    uint64_t mcfg_phys;
};
extern struct acpi_info acpi;
void acpi_init(uint64_t rsdp_phys);

/* apic.c */
void     lapic_init(void);
void     lapic_init_ap(void);
void     lapic_eoi(void);
uint32_t lapic_id(void);
void     lapic_send_init(uint32_t apic);
void     lapic_send_sipi(uint32_t apic, uint8_t page);
void     lapic_send_ipi(uint32_t apic, uint8_t vector);
void     lapic_broadcast_ipi(uint8_t vector);
void     lapic_timer_start(uint32_t hz);
void     ioapic_init(void);
void     ioapic_route_isa(int irq, int vector, uint32_t apic);
void     ioapic_route_gsi(uint32_t gsi, int vector, uint32_t apic, bool level, bool low);

/* timer.c */
extern volatile uint64_t timer_ticks_v;
#define timer_ticks timer_ticks_v
void     timer_calibrate(void);
void     timer_init(void);
void     timer_init_ap(void);
uint64_t uptime_ms(void);
void     udelay(uint64_t us);
void     mdelay(uint64_t ms);
uint64_t tsc_hz(void);

/* rtc.c */
struct datetime {
    int year, month, day, hour, minute, second, weekday;
};
void rtc_init(void);
void rtc_now(struct datetime *dt);
int64_t rtc_epoch(void);
void epoch_to_datetime(int64_t t, struct datetime *dt);

/* ps2.c */
void ps2_init(void);
bool mouse_is_absolute(void);

/* pci.c */
struct pci_dev {
    uint8_t  bus, dev, fn;
    uint16_t vendor, device;
    uint8_t  cls, subcls, progif, rev;
    uint8_t  irq_line, irq_pin;
    uint32_t bar[6];
};
void pci_init(void);
int  pci_count(void);
struct pci_dev *pci_get(int i);
struct pci_dev *pci_find_class(uint8_t cls, uint8_t sub);
struct pci_dev *pci_find(uint16_t vendor, uint16_t device);
uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off);
void     pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v);
uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off);
void     pci_write16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint16_t v);
void     pci_enable_busmaster(struct pci_dev *d);
uint64_t pci_bar_addr(struct pci_dev *d, int bar);
uint8_t  pci_find_cap(struct pci_dev *d, uint8_t id, uint8_t after);
static inline bool pci_bar_is_io(struct pci_dev *d, int bar) { return d->bar[bar] & 1; }
const char *pci_class_name(uint8_t cls, uint8_t sub);
const char *pci_vendor_name(uint16_t vendor);
const char *pci_device_name(uint16_t vendor, uint16_t device);

/* power.c */
NORETURN void system_reboot(void);
NORETURN void system_poweroff(void);

/* usb.c */
void usb_init(void);
int  usb_device_count(void);
const char *usb_device_name(int i);

/* net */
void net_init(void);

#endif

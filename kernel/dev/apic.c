/*
 * Local APIC and I/O APIC.
 */
#include <kernel.h>
#include <dev.h>
#include <cpu.h>
#include <mm.h>
#include <x86.h>

#define LAPIC_ID        0x020
#define LAPIC_VER       0x030
#define LAPIC_TPR       0x080
#define LAPIC_EOI       0x0B0
#define LAPIC_SVR       0x0F0
#define LAPIC_ESR       0x280
#define LAPIC_ICR_LO    0x300
#define LAPIC_ICR_HI    0x310
#define LAPIC_LVT_TIMER 0x320
#define LAPIC_LVT_LINT0 0x350
#define LAPIC_LVT_LINT1 0x360
#define LAPIC_LVT_ERR   0x370
#define LAPIC_TIMER_INIT 0x380
#define LAPIC_TIMER_CUR  0x390
#define LAPIC_TIMER_DIV  0x3E0

static volatile uint8_t *lapic;
static volatile uint32_t *ioapic;
static uint32_t lapic_timer_count;   /* ticks per ms, divide-by-16 */

static inline uint32_t lr(uint32_t reg) { return *(volatile uint32_t *)(lapic + reg); }
static inline void lw(uint32_t reg, uint32_t v) { *(volatile uint32_t *)(lapic + reg) = v; }

static void pic_disable(void)
{
    /* remap to 0xE0.. so stray IRQs cannot look like exceptions, then mask */
    outb(0x20, 0x11); io_wait();
    outb(0xA0, 0x11); io_wait();
    outb(0x21, 0xE0); io_wait();
    outb(0xA1, 0xE8); io_wait();
    outb(0x21, 4); io_wait();
    outb(0xA1, 2); io_wait();
    outb(0x21, 1); io_wait();
    outb(0xA1, 1); io_wait();
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
}

static void lapic_setup_this(void)
{
    lw(LAPIC_TPR, 0);
    lw(LAPIC_LVT_LINT0, 1u << 16);        /* masked */
    lw(LAPIC_LVT_LINT1, (1u << 16));
    lw(LAPIC_LVT_ERR, 0xFE);
    lw(LAPIC_ESR, 0);
    lw(LAPIC_ESR, 0);
    lw(LAPIC_SVR, 0x100 | VEC_SPURIOUS);  /* enable */
    lw(LAPIC_EOI, 0);
}

void lapic_init(void)
{
    pic_disable();
    uint64_t base = rdmsr(MSR_APIC_BASE);
    wrmsr(MSR_APIC_BASE, base | (1u << 11));   /* global enable, xAPIC mode */
    lapic = vmm_map_mmio(acpi.lapic_phys, 4096);
    lapic_setup_this();
    cpus[0].lapic_id = lapic_id();
    klog("lapic: id %u, version %#x", cpus[0].lapic_id, lr(LAPIC_VER) & 0xFF);
}

void lapic_init_ap(void)
{
    uint64_t base = rdmsr(MSR_APIC_BASE);
    wrmsr(MSR_APIC_BASE, base | (1u << 11));
    lapic_setup_this();
}

void lapic_eoi(void) { lw(LAPIC_EOI, 0); }

uint32_t lapic_id(void) { return lr(LAPIC_ID) >> 24; }

static void icr_wait(void)
{
    for (int i = 0; i < 1000000 && (lr(LAPIC_ICR_LO) & (1u << 12)); i++) cpu_relax();
}

void lapic_send_init(uint32_t apic)
{
    lw(LAPIC_ESR, 0);
    lw(LAPIC_ICR_HI, apic << 24);
    lw(LAPIC_ICR_LO, 0x00004500);          /* INIT, level assert */
    icr_wait();
}

void lapic_send_sipi(uint32_t apic, uint8_t page)
{
    lw(LAPIC_ICR_HI, apic << 24);
    lw(LAPIC_ICR_LO, 0x00004600 | page);   /* STARTUP */
    icr_wait();
}

void lapic_send_ipi(uint32_t apic, uint8_t vector)
{
    lw(LAPIC_ICR_HI, apic << 24);
    lw(LAPIC_ICR_LO, 0x00004000 | vector);
    icr_wait();
}

void lapic_broadcast_ipi(uint8_t vector)
{
    if (!lapic) return;
    lw(LAPIC_ICR_HI, 0);
    lw(LAPIC_ICR_LO, 0x000C4000 | vector); /* all excluding self */
    icr_wait();
}

/* ------------------------------------------------------------------------
 * timer
 * ---------------------------------------------------------------------- */

/* measure LAPIC ticks per millisecond using the PIT (called on the BSP) */
uint32_t pit_measure_lapic(void);

void lapic_timer_calibrate(void)
{
    lw(LAPIC_TIMER_DIV, 0x3);              /* divide by 16 */
    lapic_timer_count = pit_measure_lapic();
    if (lapic_timer_count < 100) lapic_timer_count = 100000;   /* sane fallback */
    klog("lapic: timer %u kHz (div 16)", lapic_timer_count);
}

void lapic_timer_begin_measure(void)
{
    lw(LAPIC_TIMER_DIV, 0x3);
    lw(LAPIC_LVT_TIMER, 1u << 16);         /* masked, one-shot */
    lw(LAPIC_TIMER_INIT, 0xFFFFFFFF);
}

uint32_t lapic_timer_elapsed(void)
{
    return 0xFFFFFFFF - lr(LAPIC_TIMER_CUR);
}

void lapic_timer_start(uint32_t hz)
{
    uint32_t count = lapic_timer_count * 1000 / hz;
    lw(LAPIC_TIMER_DIV, 0x3);
    lw(LAPIC_LVT_TIMER, VEC_TIMER | (1u << 17));   /* periodic */
    lw(LAPIC_TIMER_INIT, count);
}

/* ------------------------------------------------------------------------
 * I/O APIC
 * ---------------------------------------------------------------------- */

static uint32_t io_read(uint32_t reg) { ioapic[0] = reg; return ioapic[4]; }
static void io_write(uint32_t reg, uint32_t v) { ioapic[0] = reg; ioapic[4] = v; }

void ioapic_init(void)
{
    ioapic = vmm_map_mmio(acpi.ioapic_phys, 4096);
    uint32_t ver = io_read(1);
    int n = (int)((ver >> 16) & 0xFF) + 1;
    for (int i = 0; i < n; i++) {
        io_write(0x10 + 2 * i, 1u << 16);   /* masked */
        io_write(0x11 + 2 * i, 0);
    }
    klog("ioapic: %d pins, gsi base %u", n, acpi.ioapic_gsi_base);
}

void ioapic_route_gsi(uint32_t gsi, int vector, uint32_t apic, bool level, bool low)
{
    uint32_t pin = gsi - acpi.ioapic_gsi_base;
    uint32_t lo = (uint32_t)vector | (low ? (1u << 13) : 0) | (level ? (1u << 15) : 0);
    io_write(0x11 + 2 * pin, apic << 24);
    io_write(0x10 + 2 * pin, lo);
}

void ioapic_route_isa(int irq, int vector, uint32_t apic)
{
    uint32_t gsi = (uint32_t)irq;
    bool level = false, low = false;
    if (irq < 16 && acpi.iso[irq].used) {
        gsi = acpi.iso[irq].gsi;
        uint16_t f = acpi.iso[irq].flags;
        low = (f & 3) == 3;
        level = ((f >> 2) & 3) == 3;
    }
    ioapic_route_gsi(gsi, vector, apic, level, low);
}

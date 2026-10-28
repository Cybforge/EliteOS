/*
 * Time keeping: PIT-calibrated TSC and LAPIC timer. Every CPU gets a
 * 1 kHz periodic LAPIC interrupt that drives the scheduler. The global
 * millisecond counter is derived from the TSC, so it stays exact even if
 * timer interrupts are delayed.
 */
#include <kernel.h>
#include <dev.h>
#include <cpu.h>
#include <x86.h>
#include <sched.h>

volatile uint64_t timer_ticks_v;
static uint64_t tsc_base;
static uint64_t tsc_per_ms = 1000000;

void lapic_timer_begin_measure(void);
uint32_t lapic_timer_elapsed(void);
void lapic_timer_calibrate(void);

#define PIT_HZ 1193182u

/* ACPI power-management timer: 3.579545 MHz, 24 or 32 bits wide. */
static bool pmtimer_wait(uint32_t ms)
{
    if (!acpi.pm_tmr_port) return false;
    uint32_t mask = acpi.pm_tmr_32 ? 0xFFFFFFFFu : 0xFFFFFFu;
    uint32_t start = inl(acpi.pm_tmr_port) & mask;
    uint64_t need = 3579545ull * ms / 1000, elapsed = 0;
    uint32_t last = start;
    uint64_t t0 = rdtsc();
    while (elapsed < need && rdtsc() - t0 < 6000000000ull) {
        uint32_t now = inl(acpi.pm_tmr_port) & mask;
        elapsed += (now - last) & mask;
        last = now;
    }
    return elapsed >= need;
}

/* Busy-wait for `ms` milliseconds using PIT channel 2 (speaker gate).
 * Returns false if the PIT never signalled (some newer boards gate it off). */
static bool pit_wait(uint32_t ms)
{
    uint32_t count = PIT_HZ * ms / 1000;
    uint8_t p61 = inb(0x61);
    outb(0x61, (uint8_t)((p61 & ~0x02) | 0x01));   /* gate on, speaker off */
    outb(0x43, 0xB0);                               /* ch2, lo/hi, mode 0 */
    outb(0x42, (uint8_t)(count & 0xFF));
    outb(0x42, (uint8_t)(count >> 8));
    /* restart the count by toggling the gate */
    uint8_t g = inb(0x61) & (uint8_t)~0x01;
    outb(0x61, g);
    outb(0x61, g | 1);
    /* give up after ~3e9 TSC cycles (about a second on any real CPU) */
    uint64_t t0 = rdtsc();
    while (rdtsc() - t0 < 3000000000ull)
        if (inb(0x61) & 0x20) return true;
    return false;
}

static const char *calib_source = "PIT";

uint32_t pit_measure_lapic(void)
{
    const uint32_t ms = 50;
    lapic_timer_begin_measure();
    uint64_t t0 = rdtsc();
    bool ok = pit_wait(ms);
    if (!ok) {
        /* no PIT: measure again against the ACPI PM timer */
        calib_source = "ACPI PM timer";
        lapic_timer_begin_measure();
        t0 = rdtsc();
        ok = pmtimer_wait(ms);
        if (!ok) calib_source = "guess";
    }
    uint32_t el = lapic_timer_elapsed();
    uint64_t t1 = rdtsc();
    if (!ok) {
        /* last resort: CPUID leaf 0x16 base frequency, else assume 2 GHz */
        uint32_t a, b, c, d;
        cpuid(0, 0, &a, &b, &c, &d);
        uint64_t mhz = 2000;
        if (a >= 0x16) { cpuid(0x16, 0, &a, &b, &c, &d); if (a) mhz = a; }
        t1 = t0 + mhz * 1000 * ms;
        el = (uint32_t)(62500 * ms);
    }
    tsc_per_ms = (t1 - t0) / ms;
    if (tsc_per_ms < 1000) tsc_per_ms = 1000000;
    cpu_info.tsc_hz = tsc_per_ms * 1000;
    return el / ms;
}

void timer_calibrate(void)
{
    tsc_base = rdtsc();
    lapic_timer_calibrate();
    tsc_base = rdtsc();
    klog("timer: TSC %lu MHz (calibrated with the %s)", tsc_per_ms / 1000, calib_source);
}

uint64_t tsc_hz(void) { return tsc_per_ms * 1000; }

uint64_t uptime_ms(void)
{
    return (rdtsc() - tsc_base) / tsc_per_ms;
}

void udelay(uint64_t us)
{
    uint64_t end = rdtsc() + tsc_per_ms * us / 1000;
    while (rdtsc() < end) cpu_relax();
}

void mdelay(uint64_t ms) { udelay(ms * 1000); }

static void timer_irq(struct regs *r, void *ctx)
{
    UNUSED(r);
    UNUSED(ctx);
    uint64_t now = uptime_ms();
    uint64_t cur = timer_ticks_v;
    while (now > cur && !__atomic_compare_exchange_n(&timer_ticks_v, &cur, now, false,
                                                     __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
    sched_tick();
}

void timer_init(void)
{
    irq_register(VEC_TIMER, timer_irq, NULL);
    lapic_timer_start(1000);
}

void timer_init_ap(void)
{
    lapic_timer_start(1000);
}

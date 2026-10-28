/*
 * Application processor bring-up: INIT-SIPI-SIPI through a real-mode
 * trampoline at 0x8000, one CPU at a time.
 */
#include <kernel.h>
#include <cpu.h>
#include <dev.h>
#include <mm.h>
#include <x86.h>
#include <sched.h>

extern char ap_trampoline[], ap_trampoline_end[], ap_tramp_data[];

#define TRAMP_PHYS 0x8000

struct tramp_data {
    uint64_t cr3;
    uint64_t stack;
    uint64_t cpu;
    uint64_t entry;
};

static void ipi_halt(struct regs *r, void *ctx)
{
    UNUSED(r);
    UNUSED(ctx);
    for (;;) { cli(); hlt(); }
}

static void ipi_wake(struct regs *r, void *ctx)
{
    UNUSED(r);
    UNUSED(ctx);
    this_cpu()->need_resched = 1;
}

static NORETURN void ap_main(struct cpu *c)
{
    wrmsr(MSR_GS_BASE, (uint64_t)c);
    cpu_init_this(c);
    idt_load();
    syscall_init_this();
    lapic_init_ap();
    c->lapic_id = lapic_id();
    timer_init_ap();
    __atomic_store_n(&c->online, 1, __ATOMIC_RELEASE);
    sched_start_ap();
    for (;;) hlt();
}

void smp_init(void)
{
    irq_register(VEC_IPI_HALT, ipi_halt, NULL);
    irq_register(VEC_IPI_WAKE, ipi_wake, NULL);
    cpus[0].online = 1;
    if (acpi.ncpus <= 1) {
        klog("smp: single processor");
        return;
    }

    size_t tsize = (size_t)(ap_trampoline_end - ap_trampoline);
    memcpy(P2V(TRAMP_PHYS), ap_trampoline, tsize);
    struct tramp_data *td = (struct tramp_data *)P2V(TRAMP_PHYS + (ap_tramp_data - ap_trampoline));
    vmm_identity_low(true);

    uint32_t bsp = cpus[0].lapic_id;
    int n = 1;
    for (int i = 0; i < acpi.ncpus && n < MAX_CPUS; i++) {
        uint32_t id = acpi.lapic_ids[i];
        if (id == bsp) continue;
        struct cpu *c = &cpus[n];
        c->self = c;
        c->id = n;
        c->lapic_id = id;
        uint8_t *stack = kmalloc(16384);
        td->cr3 = kernel_space.pml4_phys;
        td->stack = (uint64_t)stack + 16384;
        td->cpu = (uint64_t)c;
        td->entry = (uint64_t)ap_main;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);

        lapic_send_init(id);
        mdelay(10);
        lapic_send_sipi(id, TRAMP_PHYS >> 12);
        udelay(200);
        if (!c->online) lapic_send_sipi(id, TRAMP_PHYS >> 12);
        for (int w = 0; w < 1000 && !__atomic_load_n(&c->online, __ATOMIC_ACQUIRE); w++) udelay(100);
        if (c->online) {
            n++;
        } else {
            klog("smp: CPU with APIC id %u did not start", id);
            kfree(stack);
        }
    }
    ncpus = n;
    vmm_identity_low(false);
    klog("smp: %d CPUs online", ncpus);
}

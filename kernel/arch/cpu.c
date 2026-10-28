/*
 * Per-CPU setup: GDT, TSS, IDT, feature enabling, syscall MSRs,
 * exception and IRQ dispatch.
 */
#include <kernel.h>
#include <cpu.h>
#include <x86.h>
#include <dev.h>
#include <sched.h>

struct cpu cpus[MAX_CPUS];
int ncpus = 1;
struct cpu_info cpu_info;

/* ------------------------------------------------------------------------
 * CPU identification and feature enabling
 * ---------------------------------------------------------------------- */

static void detect_features(void)
{
    uint32_t a, b, c, d, max;
    cpuid(0, 0, &max, &b, &c, &d);
    memcpy(cpu_info.vendor + 0, &b, 4);
    memcpy(cpu_info.vendor + 4, &d, 4);
    memcpy(cpu_info.vendor + 8, &c, 4);
    cpu_info.vendor[12] = 0;

    cpuid(1, 0, &a, &b, &c, &d);
    cpu_info.stepping = a & 15;
    cpu_info.model = (a >> 4) & 15;
    cpu_info.family = (a >> 8) & 15;
    if (cpu_info.family == 15) cpu_info.family += (a >> 20) & 0xFF;
    if (cpu_info.family >= 6) cpu_info.model |= ((a >> 16) & 15) << 4;
    cpu_info.sse3 = c & 1;
    cpu_info.ssse3 = (c >> 9) & 1;
    cpu_info.sse41 = (c >> 19) & 1;
    cpu_info.sse42 = (c >> 20) & 1;
    cpu_info.x2apic = (c >> 21) & 1;
    cpu_info.aes = (c >> 25) & 1;
    cpu_info.avx = (c >> 28) & 1;
    cpu_info.rdrand = (c >> 30) & 1;
    cpu_info.hypervisor = (c >> 31) & 1;
    if (max >= 7) {
        cpuid(7, 0, &a, &b, &c, &d);
        cpu_info.avx2 = (b >> 5) & 1;
    }

    uint32_t ext;
    cpuid(0x80000000, 0, &ext, &b, &c, &d);
    if (ext >= 0x80000001) {
        cpuid(0x80000001, 0, &a, &b, &c, &d);
        cpu_info.nx = (d >> 20) & 1;
        cpu_info.pdpe1gb = (d >> 26) & 1;
    }
    if (ext >= 0x80000004) {
        uint32_t *p = (uint32_t *)cpu_info.brand;
        for (uint32_t i = 0; i < 3; i++)
            cpuid(0x80000002 + i, 0, &p[i * 4], &p[i * 4 + 1], &p[i * 4 + 2], &p[i * 4 + 3]);
        cpu_info.brand[48] = 0;
        /* trim leading spaces */
        char *s = cpu_info.brand;
        while (*s == ' ') s++;
        memmove(cpu_info.brand, s, strlen(s) + 1);
    } else {
        strcpy(cpu_info.brand, cpu_info.vendor);
    }
}

static void enable_features(void)
{
    /* SSE: CR0.MP=1, EM=0, TS=0; CR4.OSFXSR, OSXMMEXCPT. Also WP and PGE. */
    uint64_t cr0 = read_cr0();
    cr0 &= ~((1ull << 2) | (1ull << 3));
    cr0 |= (1ull << 1) | (1ull << 16);
    write_cr0(cr0);
    uint64_t cr4 = read_cr4();
    cr4 |= (1ull << 9) | (1ull << 10) | (1ull << 7);
    write_cr4(cr4);
    __asm__ volatile("fninit");
    uint32_t mxcsr = 0x1F80;
    __asm__ volatile("ldmxcsr %0" :: "m"(mxcsr));

    uint64_t efer = rdmsr(MSR_EFER);
    efer |= EFER_SCE;
    if (cpu_info.nx) efer |= EFER_NXE;
    wrmsr(MSR_EFER, efer);

    /* PAT: 0=WB 1=WC 2=UC- 3=UC, repeated */
    wrmsr(MSR_PAT, 0x0001070600010706ull);
}

/* ------------------------------------------------------------------------
 * GDT / TSS
 * ---------------------------------------------------------------------- */

struct PACKED gdtr { uint16_t limit; uint64_t base; };
void gdt_flush(struct gdtr *g, uint64_t cs, uint64_t ds);

static void gdt_setup(struct cpu *c)
{
    c->gdt[0] = 0;
    c->gdt[1] = 0x00AF9A000000FFFFull;   /* 0x08 kernel code */
    c->gdt[2] = 0x00CF92000000FFFFull;   /* 0x10 kernel data */
    c->gdt[3] = 0;                        /* 0x18 unused (sysret base) */
    c->gdt[4] = 0x00CFF2000000FFFFull;   /* 0x20 user data */
    c->gdt[5] = 0x00AFFA000000FFFFull;   /* 0x28 user code */

    memset(&c->tss, 0, sizeof(c->tss));
    c->tss.iopb = sizeof(struct tss);
    c->tss.ist[0] = (uint64_t)(c->df_stack + sizeof(c->df_stack));   /* IST1: double fault/NMI */

    uint64_t base = (uint64_t)&c->tss;
    uint64_t limit = sizeof(struct tss) - 1;
    uint64_t lo = (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (0x89ull << 40) |
                  (((limit >> 16) & 0xF) << 48) | (((base >> 24) & 0xFF) << 56);
    c->gdt[6] = lo;
    c->gdt[7] = base >> 32;               /* TSS descriptor is 16 bytes */
}

void cpu_init_this(struct cpu *c)
{
    gdt_setup(c);
    struct gdtr gr = { sizeof(c->gdt) - 1, (uint64_t)c->gdt };
    gdt_flush(&gr, GDT_KCODE, GDT_KDATA);
    __asm__ volatile("ltr %w0" :: "r"((uint16_t)GDT_TSS));

    wrmsr(MSR_GS_BASE, (uint64_t)c);
    wrmsr(MSR_KERNEL_GS_BASE, 0);
    enable_features();
}

void set_kernel_stack(uint64_t top)
{
    struct cpu *c = this_cpu();
    c->tss.rsp0 = top;
    c->kstack_top = top;
}

void cpu_early_init(void)
{
    struct cpu *c = &cpus[0];
    c->self = c;
    c->id = 0;
    /* GS must point at the per-CPU block before any lock is taken */
    wrmsr(MSR_GS_BASE, (uint64_t)c);
    detect_features();
    cpu_init_this(c);
}

/* ------------------------------------------------------------------------
 * IDT
 * ---------------------------------------------------------------------- */

struct PACKED idt_entry {
    uint16_t off_lo;
    uint16_t sel;
    uint8_t ist;
    uint8_t type;
    uint16_t off_mid;
    uint32_t off_hi;
    uint32_t zero;
};

static struct idt_entry idt[256] __attribute__((aligned(16)));
static struct { irq_handler_t fn; void *ctx; } irq_handlers[256];

void idt_load(void)
{
    struct gdtr r = { sizeof(idt) - 1, (uint64_t)idt };
    __asm__ volatile("lidt %0" :: "m"(r));
}

void idt_init(void)
{
    for (int i = 0; i < 256; i++) {
        uint64_t a = isr_table[i];
        idt[i].off_lo = a & 0xFFFF;
        idt[i].sel = GDT_KCODE;
        idt[i].ist = (i == 8 || i == 2) ? 1 : 0;
        idt[i].type = 0x8E;           /* present, ring 0, interrupt gate */
        idt[i].off_mid = (a >> 16) & 0xFFFF;
        idt[i].off_hi = (uint32_t)(a >> 32);
        idt[i].zero = 0;
    }
    idt_load();
}

void irq_register(int vector, irq_handler_t h, void *ctx)
{
    irq_handlers[vector].ctx = ctx;
    irq_handlers[vector].fn = h;
}

/* ------------------------------------------------------------------------
 * dispatch
 * ---------------------------------------------------------------------- */

static const char *exc_names[32] = {
    "Divide error", "Debug", "NMI", "Breakpoint", "Overflow", "Bound range",
    "Invalid opcode", "Device not available", "Double fault", "Coprocessor overrun",
    "Invalid TSS", "Segment not present", "Stack fault", "General protection fault",
    "Page fault", "Reserved", "x87 FP error", "Alignment check", "Machine check",
    "SIMD FP error", "Virtualization", "Control protection", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved", "Hypervisor injection",
    "VMM communication", "Security", "Reserved",
};

const char *exception_name(int v) { return v < 32 ? exc_names[v] : "Interrupt"; }

void isr_dispatch(struct regs *r)
{
    struct cpu *c = this_cpu();
    uint64_t v = r->vector;

    if (v < 32) {
        if (v == 2 && panic_in_progress()) {
            for (;;) { cli(); hlt(); }
        }
        if ((r->cs & 3) && v != 2 && v != 8 && v != 18) {
            /* fault in user mode: kill the process, not the system */
            proc_fault(r);
            return;
        }
        panic_regs(r, "%s (vector %lu, error %#lx)", exc_names[v], v, r->error);
    }

    c->in_irq++;
    if (irq_handlers[v].fn)
        irq_handlers[v].fn(r, irq_handlers[v].ctx);
    c->in_irq--;
    if (v != VEC_SPURIOUS && v >= 0x20 && v < 0xE0)
        lapic_eoi();
    else if (v >= 0xF0 && v != VEC_SPURIOUS)
        lapic_eoi();

    if (c->need_resched && c->ncli == 0)
        sched_preempt();
    if (r->cs & 3) {
        void proc_check_killed(struct regs *r);
        proc_check_killed(r);
    }
}

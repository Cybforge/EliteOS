#ifndef ZENITH_CPU_H
#define ZENITH_CPU_H

#include <kernel.h>
#include <mm.h>

#define MAX_CPUS 64

#define GDT_KCODE 0x08
#define GDT_KDATA 0x10
#define GDT_UDATA 0x23
#define GDT_UCODE 0x2B
#define GDT_TSS   0x30

struct tss {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb;
} PACKED;

/* Interrupt / syscall frame (see arch/entry.S) */
struct regs {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error;
    uint64_t rip, cs, rflags, rsp, ss;
};

struct thread;

struct cpu {
    struct cpu *self;           /* gs:0  */
    uint64_t kstack_top;        /* gs:8  */
    uint64_t user_rsp;          /* gs:16 */
    struct thread *current;     /* gs:24 */
    int id;
    uint32_t lapic_id;
    int ncli;
    int intena;
    int need_resched;
    int in_irq;
    struct thread *idle;
    volatile int online;
    uint64_t slice_start;

    /* accounting, in timer ticks */
    volatile uint64_t ticks_busy;
    volatile uint64_t ticks_idle;
    uint64_t hist_busy, hist_idle;
    volatile uint32_t load;     /* percent over the last ~500 ms */
    addrspace_t *volatile active_as;   /* the address space loaded in CR3 (to know whom to tell about changed mappings) */

    uint64_t gdt[8] __attribute__((aligned(16)));
    struct tss tss __attribute__((aligned(16)));
    uint8_t df_stack[8192] __attribute__((aligned(16)));
};

extern struct cpu cpus[MAX_CPUS];
extern int ncpus;

static inline struct cpu *this_cpu(void)
{
    struct cpu *c;
    __asm__ volatile("mov %%gs:0, %0" : "=r"(c));
    return c;
}

/* CPU information gathered at boot */
struct cpu_info {
    char vendor[13];
    char brand[49];
    uint32_t family, model, stepping;
    bool sse3, ssse3, sse41, sse42, avx, avx2, aes, rdrand, hypervisor, x2apic, pdpe1gb, nx;
    uint64_t tsc_hz;
};
extern struct cpu_info cpu_info;

void cpu_early_init(void);
void cpu_init_this(struct cpu *c);
void idt_init(void);
void idt_load(void);
void syscall_init_this(void);
void smp_init(void);
void set_kernel_stack(uint64_t top);

typedef void (*irq_handler_t)(struct regs *r, void *ctx);
void irq_register(int vector, irq_handler_t h, void *ctx);

/* entry.S */
void switch_context(uint64_t *save_rsp, uint64_t new_rsp);
void thread_trampoline(void);
NORETURN void enter_user(uint64_t rip, uint64_t rsp, uint64_t a0, uint64_t a1);
void syscall_entry(void);
extern uint64_t isr_table[256];

/* interrupt vectors */
#define VEC_TIMER     0x20
#define VEC_KEYBOARD  0x21
#define VEC_MOUSE     0x2C
#define VEC_NET       0x2B
#define VEC_IPI_HALT  0xF0
#define VEC_IPI_WAKE  0xF1
#define VEC_SPURIOUS  0xFF

#endif

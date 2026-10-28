/*
 * ZenithOS low-level entry points (x86-64, GNU as, Intel syntax).
 */
.intel_syntax noprefix

#define KSTACK_SIZE (64 * 1024)

/* offsets into struct cpu (see include/cpu.h) */
#define CPU_KSTACK_TOP 8
#define CPU_USER_RSP   16

.section .bss
.align 16
.global boot_stack
boot_stack:
    .skip KSTACK_SIZE
boot_stack_top:

.section .text

/* ---------------------------------------------------------------------
 * Kernel entry from the bootloader. RDI = struct bootinfo * (HHDM).
 * ------------------------------------------------------------------- */
.global _start
_start:
    cli
    cld
    lea rsp, [rip + boot_stack_top]
    xor rbp, rbp
    call kmain
1:  hlt
    jmp 1b

/* ---------------------------------------------------------------------
 * GDT reload: gdt_flush(gdtr *, code_sel, data_sel)
 * ------------------------------------------------------------------- */
.global gdt_flush
gdt_flush:
    lgdt [rdi]
    mov ax, dx
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor eax, eax
    mov fs, ax
    /* GS selector load would clobber GS base - leave it alone */
    push rsi
    lea rax, [rip + 2f]
    push rax
    retfq
2:  ret

/* ---------------------------------------------------------------------
 * Interrupt stubs
 * ------------------------------------------------------------------- */
.macro ISR_NOERR n
isr_\n:
    push 0
    push \n
    jmp isr_common
.endm

.macro ISR_ERR n
isr_\n:
    push \n
    jmp isr_common
.endm

.altmacro
.macro ISR_ANY n
.if (\n == 8) || (\n >= 10 && \n <= 14) || (\n == 17) || (\n == 21) || (\n == 29) || (\n == 30)
    ISR_ERR \n
.else
    ISR_NOERR \n
.endif
.endm

.set i, 0
.rept 256
    ISR_ANY %i
    .set i, i + 1
.endr

.macro ISR_ADDR n
    .quad isr_\n
.endm

.section .rodata
.global isr_table
isr_table:
.set i, 0
.rept 256
    ISR_ADDR %i
    .set i, i + 1
.endr

.section .text

isr_common:
    /* [rsp] = vector, [rsp+8] = error, [rsp+16] = rip, [rsp+24] = cs */
    test qword ptr [rsp + 24], 3
    jz 1f
    swapgs
1:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    cld
    mov rdi, rsp
    sub rsp, 512
    fxsave64 [rsp]
    call isr_dispatch
    fxrstor64 [rsp]
    add rsp, 512
.global isr_return
isr_return:
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16
    test qword ptr [rsp + 8], 3
    jz 2f
    swapgs
2:
    iretq

/* ---------------------------------------------------------------------
 * switch_context(uint64_t *save_rsp, uint64_t new_rsp)
 * ------------------------------------------------------------------- */
.global switch_context
switch_context:
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    mov [rdi], rsp
    mov rsp, rsi
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

/* First code run by every new thread: r12 = fn, r13 = arg. */
.global thread_trampoline
thread_trampoline:
    and rsp, -16
    call sched_thread_started
    mov rdi, r13
    call r12
    mov rdi, rax
    call thread_exit
3:  hlt
    jmp 3b

/* ---------------------------------------------------------------------
 * enter_user(rip, rsp, arg0, arg1): drop to ring 3, never returns
 * ------------------------------------------------------------------- */
.global enter_user
enter_user:
    cli
    push 0x23           /* ss: user data */
    push rsi            /* rsp */
    push 0x202          /* rflags: IF */
    push 0x2B           /* cs: user code */
    push rdi            /* rip */
    mov rdi, rdx
    mov rsi, rcx
    xor eax, eax
    xor ebx, ebx
    xor ecx, ecx
    xor edx, edx
    xor ebp, ebp
    xor r8d, r8d
    xor r9d, r9d
    xor r10d, r10d
    xor r11d, r11d
    xor r12d, r12d
    xor r13d, r13d
    xor r14d, r14d
    xor r15d, r15d
    fninit
    push 0x1F80
    ldmxcsr [rsp]
    add rsp, 8
    swapgs
    iretq

/* ---------------------------------------------------------------------
 * enter_user_regs(struct regs *frame, const void *fxsave): continue in user mode from a prepared
 * register frame (a new thread, a forked child), with its FPU state
 * ------------------------------------------------------------------- */
.global enter_user_regs
enter_user_regs:
    cli
    fxrstor64 [rsi]
    mov rsp, rdi
    jmp isr_return

/* ---------------------------------------------------------------------
 * SYSCALL entry. rax = number, args in rdi rsi rdx r10 r8 r9.
 * Builds the same struct regs frame as interrupts.
 * ------------------------------------------------------------------- */
.global syscall_entry
syscall_entry:
    swapgs
    mov gs:[CPU_USER_RSP], rsp
    mov rsp, gs:[CPU_KSTACK_TOP]
    push 0x23                       /* ss */
    push qword ptr gs:[CPU_USER_RSP] /* rsp */
    push r11                        /* rflags */
    push 0x2B                       /* cs */
    push rcx                        /* rip */
    push 0                          /* error */
    push 0x80                       /* vector marker */
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    cld
    mov rdi, rsp
    sub rsp, 512
    fxsave64 [rsp]
    sti
    call syscall_dispatch
    cli
    fxrstor64 [rsp]
    add rsp, 512
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16                     /* vector, error */
    pop rcx                         /* rip */
    add rsp, 8                      /* cs */
    pop r11                         /* rflags */
    pop rsp                         /* user rsp */
    swapgs
    sysretq

/* ---------------------------------------------------------------------
 * AP trampoline, copied to physical 0x8000. Real mode -> long mode.
 * The data block at the end is filled in by the BSP per AP.
 * ------------------------------------------------------------------- */
#define TRAMP_BASE 0x8000
#define T(x) (TRAMP_BASE + (x - ap_trampoline))

.section .rodata
.global ap_trampoline
.global ap_trampoline_end
.code16
ap_trampoline:
    cli
    cld
    xor ax, ax
    mov ds, ax
    lgdt [T(tramp_gdtr)]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    .byte 0x66, 0xEA
    .long T(tramp_pm32)
    .word 0x08

.code32
tramp_pm32:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov eax, cr4
    or eax, (1 << 5) | (1 << 7) | (1 << 9) | (1 << 10)   /* PAE PGE OSFXSR OSXMMEXCPT */
    mov cr4, eax
    mov eax, [T(tramp_cr3)]
    mov cr3, eax
    mov ecx, 0xC0000080
    rdmsr
    or eax, (1 << 0) | (1 << 8) | (1 << 11)              /* SCE LME NXE */
    wrmsr
    mov eax, cr0
    and eax, ~((1 << 2) | (1 << 3))                      /* clear EM, TS */
    or eax, (1 << 31) | (1 << 16) | (1 << 1) | 1         /* PG WP MP PE */
    mov cr0, eax
    .byte 0xEA
    .long T(tramp_lm64)
    .word 0x18

.code64
tramp_lm64:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov rsp, [T(tramp_stack)]
    mov rdi, [T(tramp_cpu)]
    mov rax, [T(tramp_entry)]
    jmp rax

.align 16
tramp_gdt:
    .quad 0
    .quad 0x00CF9A000000FFFF     /* 0x08 code32 */
    .quad 0x00CF92000000FFFF     /* 0x10 data */
    .quad 0x00AF9A000000FFFF     /* 0x18 code64 */
tramp_gdtr:
    .word tramp_gdtr - tramp_gdt - 1
    .long T(tramp_gdt)

.align 8
.global ap_tramp_data
ap_tramp_data:
tramp_cr3:   .quad 0
tramp_stack: .quad 0
tramp_cpu:   .quad 0
tramp_entry: .quad 0
ap_trampoline_end:

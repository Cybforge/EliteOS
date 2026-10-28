#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <ucontext.h>
#include <sys/syscall.h>
#include <time.h>

/* regloop(iterations): keeps known values in the registers and checks them; returns 0 or the number of the bad one */
extern long regloop(long iterations);
asm(
".text\n.globl regloop\nregloop:\n"
"  push %rbx\n push %rbp\n push %r12\n push %r13\n push %r14\n push %r15\n"
"  sub $32, %rsp\n"
"  mov %rdi, 0(%rsp)\n"
"  mov $0x1111111111111111, %rax\n  movq %rax, %xmm0\n  mov $0x1212121212121212, %rax\n  movq %rax, %xmm1\n"
"  mov $0x1313131313131313, %rax\n  movq %rax, %xmm2\n  mov $0x1414141414141414, %rax\n  movq %rax, %xmm3\n"
"  mov $0x1515151515151515, %rax\n  movq %rax, %xmm4\n  mov $0x1616161616161616, %rax\n  movq %rax, %xmm5\n"
"  mov $0x1717171717171717, %rax\n  movq %rax, %xmm6\n  mov $0x1818181818181818, %rax\n  movq %rax, %xmm7\n"
"  mov $0x1919191919191919, %rax\n  movq %rax, %xmm8\n  mov $0x1a1a1a1a1a1a1a1a, %rax\n  movq %rax, %xmm9\n"
"  pxor %xmm15, %xmm15\n"
"  mov $0x1111111111111111, %rax\n  mov $0x2222222222222222, %rbx\n  mov $0x3333333333333333, %rcx\n  mov $0x4444444444444444, %rdx\n"
"  mov $0x5555555555555555, %rsi\n  mov $0x6666666666666666, %rdi\n  mov $0x8888888888888888, %r8\n  mov $0x9999999999999999, %r9\n"
"  mov $0xaaaaaaaaaaaaaaaa, %r10\n  mov $0xcccccccccccccccc, %r12\n  mov $0xdddddddddddddddd, %r13\n  mov $0xeeeeeeeeeeeeeeee, %r14\n"
"  mov $0xffffffffffffff01, %r15\n"
"1:\n"
"  mov $0x1111111111111111, %r11\n cmp %r11, %rax\n jne 90f\n"
"  mov $0x2222222222222222, %r11\n cmp %r11, %rbx\n jne 91f\n"
"  mov $0x3333333333333333, %r11\n cmp %r11, %rcx\n jne 92f\n"
"  mov $0x4444444444444444, %r11\n cmp %r11, %rdx\n jne 93f\n"
"  mov $0x5555555555555555, %r11\n cmp %r11, %rsi\n jne 94f\n"
"  mov $0x6666666666666666, %r11\n cmp %r11, %rdi\n jne 95f\n"
"  mov $0x8888888888888888, %r11\n cmp %r11, %r8\n jne 96f\n"
"  mov $0x9999999999999999, %r11\n cmp %r11, %r9\n jne 97f\n"
"  mov $0xaaaaaaaaaaaaaaaa, %r11\n cmp %r11, %r10\n jne 98f\n"
"  mov $0xcccccccccccccccc, %r11\n cmp %r11, %r12\n jne 99f\n"
"  mov $0xdddddddddddddddd, %r11\n cmp %r11, %r13\n jne 100f\n"
"  mov $0xeeeeeeeeeeeeeeee, %r11\n cmp %r11, %r14\n jne 101f\n"
"  mov $0xffffffffffffff01, %r11\n cmp %r11, %r15\n jne 102f\n"
"  movq %xmm0, %r11\n mov $0x1111111111111111, %rbp\n cmp %rbp, %r11\n jne 110f\n"
"  movq %xmm1, %r11\n mov $0x1212121212121212, %rbp\n cmp %rbp, %r11\n jne 111f\n"
"  movq %xmm2, %r11\n mov $0x1313131313131313, %rbp\n cmp %rbp, %r11\n jne 112f\n"
"  movq %xmm3, %r11\n mov $0x1414141414141414, %rbp\n cmp %rbp, %r11\n jne 113f\n"
"  movq %xmm4, %r11\n mov $0x1515151515151515, %rbp\n cmp %rbp, %r11\n jne 114f\n"
"  movq %xmm5, %r11\n mov $0x1616161616161616, %rbp\n cmp %rbp, %r11\n jne 115f\n"
"  movq %xmm6, %r11\n mov $0x1717171717171717, %rbp\n cmp %rbp, %r11\n jne 116f\n"
"  movq %xmm7, %r11\n mov $0x1818181818181818, %rbp\n cmp %rbp, %r11\n jne 117f\n"
"  movq %xmm8, %r11\n mov $0x1919191919191919, %rbp\n cmp %rbp, %r11\n jne 118f\n"
"  movq %xmm9, %r11\n mov $0x1a1a1a1a1a1a1a1a, %rbp\n cmp %rbp, %r11\n jne 119f\n"
"  movdqu %xmm15, 8(%rsp)\n mov 8(%rsp), %r11\n or 16(%rsp), %r11\n jnz 130f\n"
"  decq 0(%rsp)\n jnz 1b\n"
"  xor %rax, %rax\n jmp 99999f\n"
"90: mov $1, %rax\n jmp 99999f\n91: mov $2, %rax\n jmp 99999f\n92: mov $3, %rax\n jmp 99999f\n93: mov $4, %rax\n jmp 99999f\n"
"94: mov $5, %rax\n jmp 99999f\n95: mov $6, %rax\n jmp 99999f\n96: mov $8, %rax\n jmp 99999f\n97: mov $9, %rax\n jmp 99999f\n"
"98: mov $10, %rax\n jmp 99999f\n99: mov $12, %rax\n jmp 99999f\n100: mov $13, %rax\n jmp 99999f\n101: mov $14, %rax\n jmp 99999f\n"
"102: mov $15, %rax\n jmp 99999f\n"
"110: mov $20, %rax\n jmp 99999f\n111: mov $21, %rax\n jmp 99999f\n112: mov $22, %rax\n jmp 99999f\n113: mov $23, %rax\n jmp 99999f\n"
"114: mov $24, %rax\n jmp 99999f\n115: mov $25, %rax\n jmp 99999f\n116: mov $26, %rax\n jmp 99999f\n117: mov $27, %rax\n jmp 99999f\n"
"118: mov $28, %rax\n jmp 99999f\n119: mov $29, %rax\n jmp 99999f\n130: mov $40, %rax\n jmp 99999f\n"
"99999:\n"
"  add $32, %rsp\n pop %r15\n pop %r14\n pop %r13\n pop %r12\n pop %rbp\n pop %rbx\n ret\n"
);

/* like Go's asyncPreempt: the handler makes the interrupted code call this, which saves everything */
static volatile long preempts;
void count_preempt(void) { preempts++; }
extern void tramp(void);
asm(
".text\n.globl tramp\ntramp:\n"
"  pushfq\n push %rax\n push %rbx\n push %rcx\n push %rdx\n push %rsi\n push %rdi\n push %rbp\n push %r8\n push %r9\n push %r10\n push %r11\n"
"  push %r12\n push %r13\n push %r14\n push %r15\n"
"  sub $256, %rsp\n"
"  movdqu %xmm0, 0(%rsp)\n movdqu %xmm1, 16(%rsp)\n movdqu %xmm2, 32(%rsp)\n movdqu %xmm3, 48(%rsp)\n movdqu %xmm4, 64(%rsp)\n movdqu %xmm5, 80(%rsp)\n"
"  movdqu %xmm6, 96(%rsp)\n movdqu %xmm7, 112(%rsp)\n movdqu %xmm8, 128(%rsp)\n movdqu %xmm9, 144(%rsp)\n movdqu %xmm10, 160(%rsp)\n"
"  movdqu %xmm11, 176(%rsp)\n movdqu %xmm12, 192(%rsp)\n movdqu %xmm13, 208(%rsp)\n movdqu %xmm14, 224(%rsp)\n"
"  mov %rsp, %rbx\n and $-16, %rsp\n call count_preempt\n mov %rbx, %rsp\n"
"  movdqu 0(%rsp), %xmm0\n movdqu 16(%rsp), %xmm1\n movdqu 32(%rsp), %xmm2\n movdqu 48(%rsp), %xmm3\n movdqu 64(%rsp), %xmm4\n movdqu 80(%rsp), %xmm5\n"
"  movdqu 96(%rsp), %xmm6\n movdqu 112(%rsp), %xmm7\n movdqu 128(%rsp), %xmm8\n movdqu 144(%rsp), %xmm9\n movdqu 160(%rsp), %xmm10\n"
"  movdqu 176(%rsp), %xmm11\n movdqu 192(%rsp), %xmm12\n movdqu 208(%rsp), %xmm13\n movdqu 224(%rsp), %xmm14\n"
"  add $256, %rsp\n"
"  pop %r15\n pop %r14\n pop %r13\n pop %r12\n pop %r11\n pop %r10\n pop %r9\n pop %r8\n pop %rbp\n pop %rdi\n pop %rsi\n pop %rdx\n pop %rcx\n pop %rbx\n pop %rax\n popfq\n"
"  ret\n"
);

static int mode;           /* 0: plain handler, 1: inject a call like Go */
static volatile long handled;
static void on_urg(int s, siginfo_t *si, void *ctx)
{
    ucontext_t *uc = ctx;
    handled++;
    if (mode == 1) {
        greg_t sp = uc->uc_mcontext.gregs[REG_RSP] - 8;
        *(uint64_t *)sp = (uint64_t)uc->uc_mcontext.gregs[REG_RIP];
        uc->uc_mcontext.gregs[REG_RSP] = sp;
        uc->uc_mcontext.gregs[REG_RIP] = (greg_t)tramp;
    }
}

static volatile int stop;
static volatile long bad_id;
static void *worker(void *a)
{
    long r = 0;
    while (!stop && !r) r = regloop(20000);
    if (r) bad_id = r;
    return (void *)r;
}

int main(int argc, char **argv)
{
    int nthreads = argc > 1 ? atoi(argv[1]) : 3;
    mode = argc > 2 ? atoi(argv[2]) : 1;
    int secs = argc > 3 ? atoi(argv[3]) : 3;
    stack_t ss = { .ss_sp = malloc(65536), .ss_size = 65536 };
    sigaltstack(&ss, NULL);
    struct sigaction sa = { .sa_sigaction = on_urg, .sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART };
    sigaction(SIGURG, &sa, NULL);
    pthread_t t[16];
    pid_t tid[16];
    for (int i = 0; i < nthreads; i++) pthread_create(&t[i], NULL, worker, NULL);
    sleep(1);
    printf("sigstress: %d threads, mode %d (%s)\n", nthreads, mode, mode ? "inject call" : "plain handler");
    fflush(stdout);
    struct timespec t0, now;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    long sent = 0;
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - t0.tv_sec >= secs || bad_id) break;
        for (int i = 0; i < nthreads; i++) { pthread_kill(t[i], SIGURG); sent++; }
        usleep(300);
    }
    stop = 1;
    long bad = 0;
    for (int i = 0; i < nthreads; i++) { void *r; pthread_join(t[i], &r); if (r) bad = (long)r; }
    printf("signals sent %ld handled %ld preempt calls %ld -> %s (register %ld)\n", sent, handled, preempts, bad ? "REGISTERS CORRUPTED" : "all registers intact", bad);
    return bad != 0;
}

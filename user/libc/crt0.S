/* Program entry: the kernel passes argc in RDI and argv in RSI. */
    .section .text.start
    .global _start
_start:
    and $-16, %rsp
    push %rdi
    push %rsi
    call __libc_init
    pop %rsi
    pop %rdi
    call main
    mov %rax, %rdi
    call exit
1:  jmp 1b

    .section .note.GNU-stack, "", @progbits

    .section .zenith.abi,"a"
    .asciz "zenith-abi-1"

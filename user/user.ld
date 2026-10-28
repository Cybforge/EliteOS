/* User programs are static ELF executables loaded at 4 MiB. */
ENTRY(_start)

SECTIONS
{
    . = 0x400000;
    .text : { *(.text.start) *(.text .text.*) }
    . = ALIGN(4096);
    .rodata : { *(.rodata .rodata.*) }
    .zenith.abi : { KEEP(*(.zenith.abi)) }          /* says: this program uses the ZenithOS system call interface */
    . = ALIGN(4096);
    .data : { *(.data .data.*) }
    .bss : { *(COMMON) *(.bss .bss.*) }
    /DISCARD/ : { *(.eh_frame*) *(.note*) *(.comment) }
}

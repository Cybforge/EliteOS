/* Pre-rendered fonts, embedded into the kernel image. */
    .section .rodata
    .balign 16

.macro FONT name, file
    .global _font_\name
_font_\name:
    .incbin "\file"
    .balign 16
.endm

    FONT sans13, assets/fonts/sans13.zft
    FONT sans15, assets/fonts/sans15.zft
    FONT sans18, assets/fonts/sans18.zft
    FONT medium13, assets/fonts/medium13.zft
    FONT medium15, assets/fonts/medium15.zft
    FONT bold20, assets/fonts/bold20.zft
    FONT light30, assets/fonts/light30.zft
    FONT light64, assets/fonts/light64.zft
    FONT mono14, assets/fonts/mono14.zft
    FONT monob14, assets/fonts/monob14.zft
    FONT chess54, assets/fonts/chess54.zft

    .section .note.GNU-stack, "", @progbits

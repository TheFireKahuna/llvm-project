# REQUIRES: aarch64

## On ARM64, the image leaves out the sealed KCFI prefix that fills a chunk's
## first 16 bytes, as on x86-64, and the bytes before the entry are the end of
## the chunk before it. It also leaves out the 12-byte prefix after which a
## 16-byte aligned chunk's entry lies at offset 12: the entry then lies 12
## bytes past a 16-byte boundary at or past the end of what precedes it.

# RUN: llvm-mc -triple aarch64-windows-msvc %s -filetype=obj -o %t.obj
# RUN: lld-link %t.obj -machine:arm64 -guard:cf -entry:main \
# RUN:   -out:%t.exe
# RUN: llvm-objdump -s --section=.text %t.exe | FileCheck %s

## main is 20 bytes, and vdirect's entry follows it at 0x140001020. twelve's
## follows that at 0x14000102c rather than 0x14000103c.
# CHECK:      140001000 08000094 0a000094 00000090 00000091
# CHECK-NEXT: 140001010 c0035fd6 00000000 00000000 00000000
# CHECK-NEXT: 140001020 c0035fd6 00000000 00000000 1f2003d5
# CHECK-NEXT: 140001030 c0035fd6
# CHECK-NOT:  5a1c07b8

  .linktypeprefixes
        .globl @feat.00
@feat.00 = 0x800

        .def main; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,main
        .p2align 4
        .globl main
main:
        bl vdirect
        bl twelve
        adrp x0, main
        add x0, x0, :lo12:main
        ret

        .def vdirect; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,vdirect
        .p2align 4
__cfi_vdirect:
        .long 0x00401f0f
        .byte 0x0f, 0x1f, 0x80
        .long 0x071c5a06
        .byte 0xb8
        .long 0x11111111
        .globl vdirect
vdirect:
        ret

        .def twelve; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,twelve
        .p2align 4
__cfi_twelve:
        .byte 0x0f, 0x1f, 0x80
        .long 0x071c5a06
        .byte 0xb8
        .long 0x22222222
        .globl twelve
twelve:
        nop
        ret

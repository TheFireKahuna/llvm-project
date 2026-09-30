# REQUIRES: aarch64

## On ARM64 the KCFI prefix is data before the entry, with the same layout as
## on x86-64, and it is sealed the same way: the type words of a function the
## guard function table does not list become 0, and the marker stays.

# RUN: llvm-mc -triple aarch64-windows-msvc %s -filetype=obj -o %t.obj
# RUN: lld-link %t.obj -machine:arm64 -guard:cf -import-slots -entry:main \
# RUN:   -out:%t.exe
# RUN: llvm-objdump -s --section=.text %t.exe | FileCheck %s

## direct, called only directly, is sealed; listed, in the table, keeps both
## of its type words.
# CHECK:      140001000 0f1f8006 5a1c07b8 00000000 c0035fd6
# CHECK-NEXT: 140001010 0f1f4000 0f1f8006 5a1c07b8 22222222
# CHECK-NEXT: 140001020 c0035fd6

        .globl @feat.00
@feat.00 = 0x800

        .def direct; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,direct
        .p2align 4
__cfi_direct:
        .byte 0x0f, 0x1f, 0x80
        .long 0x071c5a06
        .byte 0xb8
        .long 0x11111111
        .globl direct
direct:
        ret

        .def listed; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,listed
        .p2align 4
__cfi_listed:
        .long 0x00401f0f
        .byte 0x0f, 0x1f, 0x80
        .long 0x071c5a06
        .byte 0xb8
        .long 0x22222222
        .globl listed
listed:
        ret

        .def main; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,main
        .p2align 4
        .globl main
main:
        bl direct
        adrp x0, listed
        add x0, x0, :lo12:listed
        ret

        .section .gfids$y,"dr"
        .symidx listed

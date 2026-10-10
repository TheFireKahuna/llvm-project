# REQUIRES: x86

## Where the image leaves out a sealed KCFI prefix, the bytes before the entry
## are those of what precedes the chunk. If they read as the end of a prefix
## of ours, as before f, a KCFI check would accept the sealed function, so that
## is an error. So is a start of the marker 12 bytes before the entry, as
## before g, which a check of the second type word compares.

# RUN: llvm-mc -triple x86_64-windows-msvc %s -filetype=obj -o %t.obj
# RUN: not lld-link %t.obj -guard:cf -entry:main -opt:noref \
# RUN:   -out:%t.exe 2>&1 | FileCheck %s

# CHECK:      error: cannot leave out the KCFI prefix of f:
# CHECK-SAME: the bytes before its entry read as one
# CHECK:      error: cannot leave out the KCFI prefix of g:
# CHECK-SAME: the bytes before its entry read as one

  .linktypeprefixes
        .globl @feat.00
@feat.00 = 0x800

        .def main; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,main
        .p2align 4
        .globl main
main:
        callq f
        retq
        .fill 2, 1, 0xcc
        .byte 0x5a, 0x1c, 0x07, 0xb8
        .long 0x11111111

        .def f; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,f
        .p2align 4
        .fill 4, 1, 0x90
__cfi_f:
        nopl 0x71c5a06(%rax)
        movl $0x11111111, %eax
        .globl f
f:
        retq

        .def pre; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,pre
        .p2align 4
pre:
        .fill 4, 1, 0xcc
        .byte 0x0f, 0x1f, 0x80, 0x06
        .fill 8, 1, 0xcc

        .def g; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,g
        .p2align 4
        .fill 4, 1, 0x90
__cfi_g:
        nopl 0x71c5a06(%rax)
        movl $0x22222222, %eax
        .globl g
g:
        retq

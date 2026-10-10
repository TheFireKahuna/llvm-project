# REQUIRES: x86

## A function whose KCFI prefix the image leaves out is not moved off a page's
## first bytes: no check reads a prefix of it, and its entry is where it would
## be without a prefix.

# RUN: llvm-mc -triple x86_64-windows-msvc %s -filetype=obj -o %t.obj
# RUN: lld-link %t.obj -guard:cf -entry:main -debug:symtab \
# RUN:   -out:%t.exe
# RUN: llvm-objdump -d %t.exe | FileCheck %s

## main fills .text up to 0x140002000.
# CHECK: 0000000140002000 <f>:

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
        .fill 4090, 1, 0xcc

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

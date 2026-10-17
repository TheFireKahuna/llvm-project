# REQUIRES: x86

## The image leaves out the sealed KCFI prefix of a function whose chunk is
## aligned below 16 bytes, such as one from an object built at -Os without
## Control Flow Guard's checks: the entry lands at the first offset past the end
## of what precedes the chunk that the chunk's alignment allows, and the bytes
## between the two are int3.

# RUN: llvm-mc -triple x86_64-windows-msvc %s -filetype=obj -o %t.obj
# RUN: lld-link %t.obj -guard:cf -entry:main -debug:symtab \
# RUN:   -out:%t.exe
# RUN: llvm-objdump -d %t.exe | FileCheck %s

## main ends at 0x14000100f. small's 12-byte prefix and wide's 16-byte one,
## each in a chunk aligned to 4 bytes, overlap what precedes them.
# CHECK:      14000100e: c3 retq
# CHECK-NEXT: 14000100f: cc int3
# CHECK-EMPTY:
# CHECK-NEXT: 0000000140001010 <small>:
# CHECK-NEXT: 140001010: 90 nop
# CHECK-NEXT: 140001011: c3 retq
# CHECK-NEXT: 140001012: cc int3
# CHECK-NEXT: 140001013: cc int3
# CHECK-EMPTY:
# CHECK-NEXT: 0000000140001014 <wide>:
# CHECK-NEXT: 140001014: c3 retq
# CHECK-NOT:  nopl

  .linktypeprefixes
        .globl @feat.00
@feat.00 = 0x800

        .def main; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,main
        .p2align 4
        .globl main
main:
        callq small
        callq wide
        nop
        nop
        nop
        nop
        retq

        .def small; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,small
        .p2align 2
__cfi_small:
        nopl 0x71c5a06(%rax)
        movl $0x11111111, %eax
        .globl small
small:
        nop
        retq

        .def wide; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,wide
        .p2align 2
__cfi_wide:
        .long 0x22222222
        nopl 0x71c5a06(%rax)
        movl $0x33333333, %eax
        .globl wide
wide:
        retq

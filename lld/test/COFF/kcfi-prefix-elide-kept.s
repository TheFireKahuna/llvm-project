# REQUIRES: x86

## The image keeps the sealed KCFI prefix, with type 0, of a function whose
## chunk is the first of its output section, where nothing precedes it to
## overlap; whose prefix a patchable prefix follows; or whose prefix bytes hold
## a relocation.

# RUN: llvm-mc -triple x86_64-windows-msvc %s -filetype=obj -o %t.obj
# RUN: lld-link %t.obj -guard:cf -entry:main -debug:symtab \
# RUN:   -out:%t.exe
# RUN: llvm-objdump -d %t.exe | FileCheck %s

# CHECK:      <__cfi_first>:
# CHECK-NEXT:   nopl 0x71c5a06(%rax)
# CHECK-NEXT:   movl $0x0, %eax
# CHECK-EMPTY:
# CHECK-NEXT: <first>:
# CHECK:      <__cfi_patch>:
# CHECK-NEXT:   nopl 0x71c5a06(%rax)
# CHECK-NEXT:   movl $0x0, %eax
# CHECK-NEXT:   nop
# CHECK:      <patch>:
# CHECK:      <__cfi_reloc>:
# CHECK-NEXT:   addb %al, (%rax)
# CHECK-NEXT:   addb %al, (%rax)
# CHECK-NEXT:   nopl 0x71c5a06(%rax)
# CHECK-NEXT:   movl $0x0, %eax
# CHECK-EMPTY:
# CHECK-NEXT: <reloc>:

  .linktypeprefixes
        .globl @feat.00
@feat.00 = 0x800

        .def first; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,first
        .p2align 4
        .fill 4, 1, 0x90
__cfi_first:
        nopl 0x71c5a06(%rax)
        movl $0x11111111, %eax
        .globl first
first:
        retq

        .def main; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,main
        .p2align 4
        .globl main
main:
        callq first
        callq patch
        callq reloc
        retq

        .def patch; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,patch
        .p2align 4
__cfi_patch:
        nopl 0x71c5a06(%rax)
        movl $0x22222222, %eax
        .fill 4, 1, 0x90
        .globl patch
patch:
        retq

        .def reloc; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,reloc
        .p2align 4
__cfi_reloc:
        .rva main
        nopl 0x71c5a06(%rax)
        movl $0x33333333, %eax
        .globl reloc
reloc:
        retq

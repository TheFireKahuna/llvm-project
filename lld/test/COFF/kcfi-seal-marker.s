# REQUIRES: x86

## A KCFI prefix whose marker is not that of the type definition these targets
## fix, from an object built with other options, is not ours: the linker leaves
## its type as it is when it seals the image, as it does upstream KCFI's.

# RUN: split-file %s %t.dir
# RUN: llvm-mc -triple x86_64-windows-msvc %t.dir/main.s -filetype=obj -o %t.main.obj
# RUN: llvm-mc -triple x86_64-windows-msvc %t.dir/other.s -filetype=obj -o %t.other.obj
# RUN: lld-link %t.main.obj %t.other.obj -guard:cf -entry:main \
# RUN:   -debug:symtab -out:%t.exe
# RUN: llvm-objdump -d %t.exe | FileCheck %s

## Ours is sealed, and the image leaves out its prefix.
# CHECK:      <main>:
# CHECK-NOT:    nopl
# CHECK:      <ours>:
# CHECK:      <__cfi_other>:
# CHECK-NEXT:   nopl 0x12345678(%rax)
# CHECK-NEXT:   movl $0x22222222, %eax

#--- main.s
  .linktypeprefixes
        .globl @feat.00
@feat.00 = 0x800

        .def ours; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,ours
        .p2align 4
        .fill 4, 1, 0x90
__cfi_ours:
        nopl 0x71c5a06(%rax)
        movl $0x11111111, %eax
        .globl ours
ours:
        retq

        .def main; .scl 2; .type 32; .endef
        .text
        .globl main
main:
        callq ours
        callq other
        retq

#--- other.s
  .linktypeprefixes
        .globl @feat.00
@feat.00 = 0x800

        .def other; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,other
        .p2align 4
        .fill 4, 1, 0x90
__cfi_other:
        nopl 0x12345678(%rax)
        movl $0x22222222, %eax
        .globl other
other:
        retq

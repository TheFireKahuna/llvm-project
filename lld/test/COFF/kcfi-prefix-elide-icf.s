# REQUIRES: x86

## Identical code folding compares the prefixes as the objects hold them, so
## two identical functions that no pointer reaches fold into one chunk, which
## is sealed, and the image leaves out its prefix; both names reach its entry.

# RUN: llvm-mc -triple x86_64-windows-msvc %s -filetype=obj -o %t.obj
# RUN: lld-link %t.obj -guard:cf -entry:main -debug:symtab \
# RUN:   -opt:icf -out:%t.exe
# RUN: llvm-objdump -d %t.exe | FileCheck %s

# CHECK:      <main>:
# CHECK-NEXT:   callq 0x140001010 <f{{[12]}}>
# CHECK-NEXT:   callq 0x140001010 <f{{[12]}}>
# CHECK-NOT:    nopl
# CHECK:      0000000140001010 <f{{[12]}}>:
# CHECK-NEXT:   movl $0x2a, %eax

  .linktypeprefixes
        .globl @feat.00
@feat.00 = 0x800

        .def main; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,main
        .p2align 4
        .globl main
main:
        callq f1
        callq f2
        retq
        .fill 5, 1, 0xcc

        .def f1; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,f1
        .p2align 4
        .fill 4, 1, 0x90
__cfi_f1:
        nopl 0x71c5a06(%rax)
        movl $0x11111111, %eax
        .globl f1
f1:
        movl $42, %eax
        retq

        .def f2; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,f2
        .p2align 4
        .fill 4, 1, 0x90
__cfi_f2:
        nopl 0x71c5a06(%rax)
        movl $0x11111111, %eax
        .globl f2
f2:
        movl $42, %eax
        retq

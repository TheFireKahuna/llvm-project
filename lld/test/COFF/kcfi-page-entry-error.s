# REQUIRES: x86

## A chunk whose alignment leaves the entry of a function
## with a KCFI prefix in a page's first 16 bytes wherever it goes is an error,
## since a KCFI check would treat the function as foreign. A function no
## pointer may reach, whose prefix the linker seals, may stay there.

# RUN: llvm-mc -triple x86_64-windows-msvc %s -filetype=obj -o %t.obj
# RUN: not lld-link %t.obj -entry:main -out:%t.exe 2>&1 \
# RUN:   | FileCheck %s
# RUN: lld-link %t.obj -guard:cf -entry:main -out:%t.sealed.exe \
# RUN:   2>&1 | FileCheck %s --check-prefix=SEALED --allow-empty

# CHECK: error: cannot place f so that its functions with a KCFI prefix start past the first bytes of a page
# SEALED-NOT: error:

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

        .def f; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,f
        .p2align 12
__cfi_f:
        nopl 0x71c5a06(%rax)
        movl $0x11111111, %eax
        .globl f
f:
        retq

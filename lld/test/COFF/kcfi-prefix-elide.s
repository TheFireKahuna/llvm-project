# REQUIRES: x86

## Under -guard:cf, the image leaves out the KCFI prefix of
## a sealed function where the prefix and its padding fill the chunk's first
## alignment unit of 16 bytes or more: the chunk starts that unit early, over
## the end of what precedes it, so that its entry lands where it would without
## a prefix, and the bytes between the two are int3. A reference through the
## section with an addend, and the unwind data, reach the same entry.

# RUN: llvm-mc -triple x86_64-windows-msvc %s -filetype=obj -o %t.obj
# RUN: llvm-readobj -r %t.obj | FileCheck %s --check-prefix=OBJ
# RUN: lld-link %t.obj -guard:cf -entry:main -debug:symtab \
# RUN:   -out:%t.exe
# RUN: llvm-objdump -d %t.exe | FileCheck %s
# RUN: llvm-objdump -s -j .rdata %t.exe | FileCheck %s --check-prefix=RVA
# RUN: llvm-readobj --unwind %t.exe | FileCheck %s --check-prefix=UNWIND

## The reference to b's interior is to its section, with an addend of 17.
# OBJ:      Section ({{[0-9]+}}) .rdata {
# OBJ-NEXT:   0x0 IMAGE_REL_AMD64_ADDR32NB .text ({{[0-9]+}})

## a's prefix overlaps the end of main, which ends at its alignment, and b's
## that of a, with int3 between them. c is 32-byte aligned, its prefix after
## 20 bytes of padding. Each entry is where it would be without a prefix.
# CHECK:      0000000140001000 <main>:
# CHECK:      14000100f: c3 retq
# CHECK-EMPTY:
# CHECK-NEXT: 0000000140001010 <a>:
# CHECK-NEXT: 140001010: 53 pushq %rbx
# CHECK-NEXT: 140001011: 5b popq %rbx
# CHECK-NEXT: 140001012: c3 retq
# CHECK-COUNT-13: int3
# CHECK-EMPTY:
# CHECK-NEXT: 0000000140001020 <b>:
# CHECK-NEXT: 140001020: 90 nop
# CHECK-NEXT: 140001021: c3 retq
# CHECK-COUNT-30: int3
# CHECK-EMPTY:
# CHECK-NEXT: 0000000140001040 <c>:
# CHECK-NEXT: 140001040: c3 retq

# RVA: 140002000 21100000

# UNWIND:      StartAddress: a (0x140001010)
# UNWIND-NEXT: EndAddress: (0x140001013)

  .linktypeprefixes
        .globl @feat.00
@feat.00 = 0x800

        .def main; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,main
        .p2align 4
        .globl main
main:
        callq a
        callq b
        callq c
        retq

        .def a; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,a
        .p2align 4
        .fill 4, 1, 0x90
__cfi_a:
        nopl 0x71c5a06(%rax)
        movl $0x11111111, %eax
        .globl a
a:
.seh_proc a
        pushq %rbx
.seh_pushreg %rbx
.seh_endprologue
        popq %rbx
        retq
.seh_endproc

        .def b; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,b
        .p2align 4
__cfi_b:
        .long 0x22222222
        nopl 0x71c5a06(%rax)
        movl $0x33333333, %eax
        .globl b
b:
        nop
.Lb_ret:
        retq

        .def c; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,c
        .p2align 5
        .fill 20, 1, 0x90
__cfi_c:
        nopl 0x71c5a06(%rax)
        movl $0x44444444, %eax
        .globl c
c:
        retq

        .section .rdata,"dr"
        .rva .Lb_ret

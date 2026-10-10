# REQUIRES: x86
# RUN: llvm-mc -triple x86_64-windows-msvc -filetype=obj -o %t.obj %s
# RUN: lld-link %t.obj -guard:cf,nolongjmp -out:%t.exe -entry:main -opt:noref \
# RUN:   -map:%t.map
# RUN: llvm-readobj --coff-load-config %t.exe > %t.txt
# RUN: cat %t.map %t.txt | FileCheck %s

# An object without guard metadata has every function it references listed as
# address-taken, except one that only its unwind data names.

# CHECK:      main 00000001[[MAIN:[0-9a-f]+]]
# CHECK-NEXT: taken 00000001[[TAKEN:[0-9a-f]+]]
# CHECK-NEXT: unwound
# CHECK:      GuardCFFunctionCount: 2
# CHECK:      GuardFidTable [
# CHECK-NEXT:   0x1[[MAIN]]
# CHECK-NEXT:   0x1[[TAKEN]]
# CHECK-NEXT: ]

        .def     @feat.00; .scl    3; .type   0; .endef
        .globl  @feat.00
@feat.00 = 0

        .def     main; .scl    2; .type   32; .endef
        .section        .text,"xr",one_only,main
        .globl  main
main:
        leaq taken(%rip), %rax
        retq

        .def     taken; .scl    2; .type   32; .endef
        .section        .text,"xr",one_only,taken
        .globl  taken
        .p2align 4
taken:
.seh_proc taken
        pushq %rbp
.seh_pushreg %rbp
.seh_endprologue
        popq %rbp
        retq
.seh_endproc

        .def     unwound; .scl    2; .type   32; .endef
        .section        .text,"xr",one_only,unwound
        .globl  unwound
        .p2align 4
unwound:
.seh_proc unwound
        pushq %rbp
.seh_pushreg %rbp
.seh_endprologue
        popq %rbp
        retq
.seh_endproc

        .section .rdata,"dr"
        .p2align 3
.globl _load_config_used
_load_config_used:
        .long 256
        .fill 124, 1, 0
        .quad __guard_fids_table
        .quad __guard_fids_count
        .long __guard_flags
        .fill 128, 1, 0

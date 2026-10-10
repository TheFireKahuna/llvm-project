# REQUIRES: x86
# RUN: split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -triple x86_64-windows-msvc -filetype=obj -o guard.obj guard.s
# RUN: llvm-mc -triple x86_64-windows-msvc -filetype=obj -o lister.obj lister.s
# RUN: llvm-mc -triple x86_64-windows-msvc -filetype=obj -o stubs.obj stubs.s
# RUN: lld-link guard.obj lister.obj stubs.obj -guard:cf,nolongjmp \
# RUN:   -out:out.exe -entry:main -opt:noref -map:out.map
# RUN: llvm-readobj --coff-load-config out.exe > out.txt
# RUN: cat out.map out.txt | FileCheck %s

# A function whose address only initializes a guard pointer in .00cfg is not
# listed in the guard function table, whether its object lists it in .gfids$y
# or has no guard metadata. A function that the same object references from
# another section, or that another object lists, stays.

# CHECK:      main 00000001[[MAIN:[0-9a-f]+]]
# CHECK:      check_nop
# CHECK-NEXT: dispatch_nop
# CHECK-NEXT: kept 00000001[[KEPT:[0-9a-f]+]]
# CHECK-NEXT: listed 00000001[[LISTED:[0-9a-f]+]]
# CHECK-NEXT: plain_nop
# CHECK-NEXT: plain_kept 00000001[[PLAINKEPT:[0-9a-f]+]]
# CHECK:      GuardCFFunctionCount: 4
# CHECK:      GuardFidTable [
# CHECK-NEXT:   0x1[[MAIN]]
# CHECK-NEXT:   0x1[[KEPT]]
# CHECK-NEXT:   0x1[[LISTED]]
# CHECK-NEXT:   0x1[[PLAINKEPT]]
# CHECK-NEXT: ]

#--- guard.s
# The guard pointers, in an object with guard metadata, as Microsoft's CRT
# defines them.
        .def     @feat.00; .scl    3; .type   0; .endef
        .globl  @feat.00
@feat.00 = 0x800

        .def     main; .scl    2; .type   32; .endef
        .section        .text,"xr",one_only,main
        .globl  main
main:
        leaq kept(%rip), %rax
        callq *__guard_dispatch_icall_fptr(%rip)
        retq

        .section .00cfg,"dr"
        .p2align 3
        .globl __guard_check_icall_fptr
__guard_check_icall_fptr:
        .quad check_nop
        .globl __guard_dispatch_icall_fptr
__guard_dispatch_icall_fptr:
        .quad dispatch_nop
        .globl kept_fptr
kept_fptr:
        .quad kept
        .globl listed_fptr
listed_fptr:
        .quad listed

        .section .gfids$y,"dr"
        .symidx check_nop
        .symidx dispatch_nop
        .symidx kept
        .symidx listed

        .section .rdata,"dr"
        .p2align 3
        .globl _load_config_used
_load_config_used:
        .long 256
        .fill 108, 1, 0
        .quad __guard_check_icall_fptr
        .quad __guard_dispatch_icall_fptr
        .quad __guard_fids_table
        .quad __guard_fids_count
        .long __guard_flags
        .fill 128, 1, 0

#--- lister.s
# Another object that takes the address of a function that a guard pointer
# holds.
        .def     @feat.00; .scl    3; .type   0; .endef
        .globl  @feat.00
@feat.00 = 0x800

        .section .gfids$y,"dr"
        .symidx listed

#--- stubs.s
# An object without guard metadata, whose relocations decide what it takes the
# address of.
        .def     @feat.00; .scl    3; .type   0; .endef
        .globl  @feat.00
@feat.00 = 0

        .macro func name
        .def     \name; .scl    2; .type   32; .endef
        .section        .text,"xr",one_only,\name
        .globl  \name
        .p2align 4
\name:
        .endm

        func check_nop
        retq
        func dispatch_nop
        jmpq *%rax
        func kept
        retq
        func listed
        retq
        func plain_nop
        retq
        func plain_kept
        retq
        func user
        leaq plain_kept(%rip), %rax
        retq

        .section .00cfg,"dr"
        .p2align 3
        .globl plain_nop_fptr
plain_nop_fptr:
        .quad plain_nop
        .globl plain_kept_fptr
plain_kept_fptr:
        .quad plain_kept

# REQUIRES: x86

## The record of a DLL's KCFI code range lists the second types of the
## unsealed functions in it that can occupy a vtable slot, not the membership
## tags that LTO puts in the same word, which the objects' records tell apart.

# RUN: split-file %s %t.dir
# RUN: llvm-mc -triple x86_64-windows-msvc %t.dir/dll.s -filetype=obj -o %t.dll.obj
# RUN: lld-link %t.dll.obj -dll -noentry -guard:cf -out:%t.dll \
# RUN:   -implib:%t.lib
# RUN: llvm-readobj --sections --section-data %t.lib | FileCheck %s

## Types 0x33333333 and 0x44444444 have one function each, and second type
## 0x66666666 one; tag 0x77777777 is not listed.
# CHECK:      Name: .llvm_link_records
# CHECK:      SectionData (
# CHECK-NEXT:   0000: 4C4C5243 01000C11 02333333 33014444  |LLRC.....3333.DD|
# CHECK-NEXT:   0010: 44440101 66666666 01                 |DD..ffff.|

#--- dll.s
  .linktypeprefixes
        .globl @feat.00
@feat.00 = 0x800

        .def vtaken; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,vtaken
        .p2align 4
__cfi_vtaken:
        .long 0x66666666
        nopl 0x71c5a06(%rax)
        movl $0x33333333, %eax
        .globl vtaken
vtaken:
        retq

        .def tagged; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,tagged
        .p2align 4
__cfi_tagged:
        .linkkcfitag __cfi_tagged
        .long 0x77777777
        nopl 0x71c5a06(%rax)
        movl $0x44444444, %eax
        .globl tagged
tagged:
        movl $1, %eax
        retq

        .data
        .quad vtaken
        .quad tagged

        .section .gfids$y,"dr"
        .symidx vtaken
        .symidx tagged

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

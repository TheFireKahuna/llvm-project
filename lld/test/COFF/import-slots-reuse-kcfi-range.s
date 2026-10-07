# REQUIRES: x86

## Under -import-slots, the bounds of a DLL's KCFI code range take a 16-byte
## block of the importer's import address table, so a table that starts at an
## odd entry needs one entry before them. When every other import from that
## DLL is kept only in slots, the first of them keeps its entry for that.
## d0's table takes three entries, so d1's starts at an odd one, and d1f0,
## which code calls and .rdata holds, keeps its entry before the bounds.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -triple x86_64-windows-msvc d0.s -filetype=obj -o d0.obj
# RUN: lld-link d0.obj -dll -noentry -export:a -export:b -import-slots \
# RUN:   -out:d0.dll -implib:d0.lib
# RUN: llvm-mc -triple x86_64-windows-msvc d1.s -filetype=obj -o d1.obj
# RUN: lld-link d1.obj -dll -noentry -export:d1f0 -guard:cf -import-slots \
# RUN:   -out:d1.dll -implib:d1.lib
# RUN: llvm-mc -triple x86_64-windows-itanium exe.s -filetype=obj -o exe.obj
# RUN: lld-link exe.obj d0.lib d1.lib -guard:cf -import-slots -opt:noref \
# RUN:   -entry:main -out:exe.exe 2>&1 | count 0
# RUN: llvm-readobj --coff-imports exe.exe | FileCheck %s

# CHECK:      Name: d0.dll
# CHECK-NEXT: ImportLookupTableRVA:
# CHECK-NEXT: ImportAddressTableRVA: 0x[[#%X,D0:]]
# CHECK-NEXT: Symbol: a
# CHECK-NEXT: Symbol: b
# CHECK-NEXT: }
# CHECK:      Name: d1.dll
# CHECK-NEXT: ImportLookupTableRVA:
# CHECK-NEXT: ImportAddressTableRVA: 0x[[#%X,D0 + 24]]
# CHECK-NEXT: Symbol: d1f0
# CHECK-NEXT: Symbol: __llvm_code_start
# CHECK-NEXT: Symbol: __llvm_code_end
# CHECK-NEXT: }
# CHECK:      Name: d1.dll
# CHECK-NEXT: ImportLookupTableRVA:
# CHECK-NEXT: ImportAddressTableRVA:
# CHECK-NEXT: Symbol: d1f0
# CHECK-NEXT: }

#--- d0.s
        .text
        .globl a
a:
        retq
        .globl b
b:
        retq

#--- d1.s
        .globl @feat.00
@feat.00 = 0x800

        .def d1f0; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,d1f0
        .p2align 4
        .fill 4, 1, 0x90
__cfi_d1f0:
        nopl 0x71c5a06(%rax)
        movl $0x11111111, %eax
        .globl d1f0
d1f0:
        retq

        .section .gfids$y,"dr"
        .symidx d1f0

        .section .rdata,"dr"
        .p2align 3
        .globl _load_config_used
_load_config_used:
        .long 256
        .fill 124, 1, 0
        .quad __guard_fids_table
        .quad __guard_fids_count
        .long __guard_flags
        .fill 12, 1, 0
        .quad __guard_iat_table
        .quad __guard_iat_count
        .fill 100, 1, 0

#--- exe.s
        .globl @feat.00
@feat.00 = 0x800

        .def main; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,main
        .p2align 4
        .globl main
main:
        callq *__imp_a(%rip)
        callq *__imp_b(%rip)
        callq *__imp_d1f0(%rip)
        callq __llvm_kcfi_dispatch_11111111
        retq

        .section .rdata,"dr",one_only,table
        .p2align 3
        .globl table
table:
        .quad d1f0

        .weak __llvm_code_start
__llvm_code_start = __llvm_code_empty
        .weak __llvm_code_end
__llvm_code_end = __llvm_code_empty
        .section .rdata,"dr",discard,__llvm_code_empty
        .globl __llvm_code_empty
__llvm_code_empty:
        .byte 0

        .weak __llvm_kcfi_mismatch_11111111
__llvm_kcfi_mismatch_11111111 = __llvm_kcfi_open
        .section .text,"xr",discard,__llvm_kcfi_dispatch_11111111
        .globl __llvm_kcfi_dispatch_11111111
        .p2align 4
__llvm_kcfi_dispatch_11111111:
        leaq __llvm_code_start(%rip), %r10
        cmpq %r10, %rax
        jb 1f
        leaq __llvm_code_end(%rip), %r10
        cmpq %r10, %rax
        jae 1f
        movabsq $0x11111111b8071c5a, %r11
        cmpq %r11, -8(%rax)
        jne __llvm_kcfi_mismatch_11111111
        jmpq *%rax
1:      testl $0xff0, %eax
        je __llvm_kcfi_mismatch_11111111
        movabsq $0x11111111b8071c5a, %r11
        cmpq %r11, -8(%rax)
        jne __llvm_kcfi_mismatch_11111111
        jmpq *__guard_dispatch_icall_fptr(%rip)

        .section .text,"xr",discard,__llvm_kcfi_open
        .globl __llvm_kcfi_open
        .p2align 4
__llvm_kcfi_open:
        int3

        .section .rdata,"dr"
        .globl __guard_dispatch_icall_fptr
__guard_dispatch_icall_fptr:
        .quad 0

        .section .rdata,"dr"
        .p2align 3
        .globl _load_config_used
_load_config_used:
        .long 256
        .fill 124, 1, 0
        .quad __guard_fids_table
        .quad __guard_fids_count
        .long __guard_flags
        .fill 12, 1, 0
        .quad __guard_iat_table
        .quad __guard_iat_count
        .fill 100, 1, 0

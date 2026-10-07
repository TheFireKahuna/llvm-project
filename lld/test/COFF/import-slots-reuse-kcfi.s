# REQUIRES: x86

## Under -import-slots, a KCFI open routine reads an import's address through
## its list, which the linker or the compiler writes. An import that a list
## names is kept in a read-only slot, which the list then names, or keeps its
## import address table entry: rw, held only in writable data, keeps its entry,
## and ro, and lst, which the compiler's list names, are kept in their slots in
## .rdata.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc main.s -o main.obj
# RUN: llvm-dlltool -m i386:x86-64 -d lib.def -D lib.dll -l lib.lib
# RUN: lld-link main.obj lib.lib -import-slots -entry:main -debug:symtab \
# RUN:   -opt:noref -out:main.exe
# RUN: llvm-readobj --coff-imports main.exe | \
# RUN:   FileCheck %s --check-prefix=IMPORTS
# RUN: llvm-nm -n main.exe | FileCheck %s --check-prefix=SYMS
# RUN: llvm-objdump -s -j .rdata main.exe | FileCheck %s --check-prefix=DATA

# IMPORTS:      Name: lib.dll
# IMPORTS-NEXT: ImportLookupTableRVA:
# IMPORTS-NEXT: ImportAddressTableRVA: 0x20A0
# IMPORTS-NEXT: Symbol: rw (0)
# IMPORTS-NEXT: }

# SYMS:      1400020a0 R __imp_rw
# SYMS:      1400020b0 R __imp_ro
# SYMS-NEXT: 1400020b8 R __imp_lst

## The linker's list: its head, ro's slot, rw's entry and the trailer; no
## thunk is in the image, so neither import has a cell holding one. Then the
## compiler's list entry, which names lst's slot.
# DATA: 140002000 11111111 00000000 b0200040 01000000
# DATA: 140002010 a0200040 01000000 23222222 00000000
# DATA: 140002020 b8200040 01000000

#--- lib.def
LIBRARY lib.dll
EXPORTS
lst
ro
rw

#--- main.s
        .text
        .globl main
        .p2align 4
main:
        callq __llvm_kcfi_dispatch_11111111
        retq

        .data
        .quad rw

        .section .rdata,"dr"
        .p2align 3
        .quad ro
        .quad lst

        .section .rdata$llvm_kcfi_22222222_m,"dr"
        .p2align 3
        .quad __imp_lst

        .weak __kcfi_typeid_rw
__kcfi_typeid_rw = 0x11111111
        .weak __kcfi_typeid_ro
__kcfi_typeid_ro = 0x11111111
        .weak __llvm_kcfi_mismatch_11111111
__llvm_kcfi_mismatch_11111111 = __llvm_kcfi_trap

        .section .text,"xr",discard,__llvm_kcfi_dispatch_11111111
        .globl __llvm_kcfi_dispatch_11111111
        .p2align 4
__llvm_kcfi_dispatch_11111111:
        cmpl $0x11111111, -4(%rax)
        jne __llvm_kcfi_mismatch_11111111
        jmpq *%rax

        .section .text,"xr",discard,__llvm_kcfi_trap
        .globl __llvm_kcfi_trap
        .p2align 4
__llvm_kcfi_trap:
        movl $64, %ecx
        int $0x29

        .section .text,"xr",discard,__llvm_kcfi_open
        .globl __llvm_kcfi_open
        .p2align 4
__llvm_kcfi_open:
        int3

        .section .text,"xr",discard,__llvm_kcfi_open_dynamic
        .globl __llvm_kcfi_open_dynamic
        .p2align 4
__llvm_kcfi_open_dynamic:
        int3

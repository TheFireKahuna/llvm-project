# REQUIRES: x86

## Under -import-slots, when an object already opened a KCFI type statically,
## with its own mismatch routine, list head, entries and trailer, the linker
## keeps that routine and adds its entries for the type's foreign targets
## after the compiler's, before the trailer.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc plain.s -o plain.obj
# RUN: llvm-dlltool -m i386:x86-64 -d lib.def -D lib.dll -l lib.lib
# RUN: lld-link main.obj plain.obj lib.lib -import-slots -entry:main \
# RUN:   -debug:symtab -opt:ref -out:main.exe
# RUN: llvm-objdump -d main.exe | FileCheck %s
# RUN: llvm-objdump -s -j .rdata main.exe | FileCheck %s --check-prefix=DATA

## In MinGW mode the list's sections keep their suffixes, and so their order.
# RUN: lld-link main.obj plain.obj lib.lib -lldmingw -import-slots \
# RUN:   -entry:main -debug:symtab -opt:ref -out:mingw.exe
# RUN: llvm-objdump -s -j .rdata mingw.exe | FileCheck %s --check-prefix=MINGW

# CHECK:      <main>:
# CHECK-NEXT:   movq {{.*}} # 0x140002060 <__imp_known>
# CHECK:      <__llvm_kcfi_mismatch_33333333>:
# CHECK-NEXT:   leaq 0xffc(%rip), %r10 # 0x140002010
# CHECK-NEXT:   jmp {{.*}} <__llvm_kcfi_open>
# CHECK:      <__llvm_kcfi_dispatch_33333333>:
# CHECK:        jne 0x14000100d <__llvm_kcfi_mismatch_33333333>
# CHECK:      <plain>:
# CHECK-NEXT:   140001034:

## plain's cell, then the head, the compiler's entry for __imp_known, the
## linker's for plain's cell, and the trailer.
# DATA:      140002000 34100040 01000000 33333333 00000000
# DATA-NEXT: 140002010 60200040 01000000 00200040 01000000
# DATA-NEXT: 140002020 67666666 00000000

## The same in MinGW mode, where the constructor lists, which also go in
## .rdata, move the import table.
# MINGW:      140002000 34100040 01000000 33333333 00000000
# MINGW-NEXT: 140002010 80200040 01000000 00200040 01000000
# MINGW-NEXT: 140002020 67666666 00000000

#--- lib.def
LIBRARY lib.dll
EXPORTS
known

#--- plain.s
        .text
        .globl plain
plain:
        retq

#--- main.s
        .def main; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,main
        .globl main
        .p2align 4
main:
        movq __imp_known(%rip), %rax
        callq __llvm_kcfi_dispatch_33333333
        retq

        .data
        .quad plain

        .weak __kcfi_typeid_plain
__kcfi_typeid_plain = 0x33333333

        .section .text,"xr",largest,__llvm_kcfi_mismatch_33333333
        .globl __llvm_kcfi_mismatch_33333333
__llvm_kcfi_mismatch_33333333:
        leaq __llvm_kcfi_list_33333333+8(%rip), %r10
        jmp __llvm_kcfi_open

        .section .rdata$llvm_kcfi_33333333_a,"dr",discard,__llvm_kcfi_list_33333333
        .globl __llvm_kcfi_list_33333333
        .p2align 3
__llvm_kcfi_list_33333333:
        .quad 0x33333333
        .section .rdata$llvm_kcfi_33333333_m,"dr"
        .p2align 3
        .quad __imp_known
        .section .rdata$llvm_kcfi_33333333_z,"dr",associative,__llvm_kcfi_list_33333333
        .p2align 3
        .quad 0x66666667

        .section .text,"xr",discard,__llvm_kcfi_dispatch_33333333
        .globl __llvm_kcfi_dispatch_33333333
        .p2align 4
__llvm_kcfi_dispatch_33333333:
        cmpl $0x33333333, -4(%rax)
        jne __llvm_kcfi_mismatch_33333333
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

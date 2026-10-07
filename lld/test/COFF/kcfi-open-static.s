# REQUIRES: x86

## Under -import-slots, when __kcfi_typeid_<f> gives a KCFI type and f
## resolves to an import or to a definition without a KCFI prefix, the linker
## opens the type statically: a mismatch routine that is still the trap
## becomes one that points R10 at the type's list and jumps to the static
## scanner, and the list gets a head, a trailer and an entry for each such f.
## An import is listed by its import address table entry, and by a cell
## holding its thunk where static data holds the thunk as its address: here
## for imported, which an object without site records calls, and not for
## imported2, whose thunk is not in the image. A definition without a prefix
## is listed by a cell holding its address. A definition with a prefix is not
## listed, and a type without such a target keeps the trap.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc plain.s -o plain.obj
# RUN: llvm-dlltool -m i386:x86-64 -d lib.def -D lib.dll -l lib.lib
# RUN: lld-link main.obj plain.obj lib.lib -import-slots -entry:main \
# RUN:   -debug:symtab -opt:ref -out:main.exe
# RUN: llvm-objdump -d main.exe | FileCheck %s
# RUN: llvm-objdump -s -j .rdata main.exe | FileCheck %s --check-prefix=DATA
# RUN: llvm-readobj --coff-basereloc main.exe | \
# RUN:   FileCheck %s --check-prefix=RELOC

# CHECK:      <__llvm_kcfi_dispatch_11111111>:
# CHECK:        jne 0x140001075 <__llvm_kcfi_mismatch_11111111>
# CHECK:      <__llvm_kcfi_dispatch_22222222>:
# CHECK:        jne 0x140001060 <__llvm_kcfi_trap>
# CHECK:      <__llvm_kcfi_open>:
# CHECK:      <plain>:
# CHECK:      <__llvm_kcfi_mismatch_11111111>:
# CHECK-NEXT:   4c 8d 15 9c 0f 00 00 leaq 0xf9c(%rip), %r10 # 0x140002018
# CHECK-NEXT:   e9 ef ff ff ff       jmp 0x140001070 <__llvm_kcfi_open>
# CHECK:      <imported>:
# CHECK-NEXT:   140001090:

## The cells come first: plain's, then imported's thunk's. Then the head, the
## entries, __imp_imported, __imp_imported2, plain's cell and the thunk's cell,
## and the trailer, before the import tables.
# DATA:      140002000 74100040 01000000 90100040 01000000
# DATA-NEXT: 140002010 11111111 00000000 80200040 01000000
# DATA-NEXT: 140002020 88200040 01000000 00200040 01000000
# DATA-NEXT: 140002030 08200040 01000000 23222222 00000000

## Every list word and cell has a base relocation.
# RELOC:     Address: 0x2000
# RELOC:     Address: 0x2008
# RELOC:     Address: 0x2018
# RELOC:     Address: 0x2020
# RELOC:     Address: 0x2028
# RELOC:     Address: 0x2030

## Without -import-slots, the routine stays the trap.
# RUN: lld-link main.obj plain.obj lib.lib -entry:main -debug:symtab \
# RUN:   -opt:ref -out:plain.exe
# RUN: llvm-objdump -d plain.exe | FileCheck %s --check-prefix=PLAIN

# PLAIN:     <__llvm_kcfi_dispatch_11111111>:
# PLAIN:       jne {{.*}} <__llvm_kcfi_trap>
# PLAIN-NOT: <__llvm_kcfi_mismatch_11111111>:

#--- lib.def
LIBRARY lib.dll
EXPORTS
imported
imported2

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
        movq __imp_imported2(%rip), %rax
        callq __llvm_kcfi_dispatch_11111111
        callq __llvm_kcfi_dispatch_22222222
        callq imported
        retq

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

        .data
        .quad imported
        .quad plain
        .quad ours

        .weak __kcfi_typeid_imported
__kcfi_typeid_imported = 0x11111111
        .weak __kcfi_typeid_imported2
__kcfi_typeid_imported2 = 0x11111111
        .weak __kcfi_typeid_plain
__kcfi_typeid_plain = 0x11111111
        .weak __kcfi_typeid_ours
__kcfi_typeid_ours = 0x11111111

        .weak __llvm_kcfi_mismatch_11111111
__llvm_kcfi_mismatch_11111111 = __llvm_kcfi_trap
        .weak __llvm_kcfi_mismatch_22222222
__llvm_kcfi_mismatch_22222222 = __llvm_kcfi_trap

        .section .text,"xr",discard,__llvm_kcfi_dispatch_11111111
        .globl __llvm_kcfi_dispatch_11111111
        .p2align 4
__llvm_kcfi_dispatch_11111111:
        cmpl $0x11111111, -4(%rax)
        jne __llvm_kcfi_mismatch_11111111
        jmpq *%rax

        .section .text,"xr",discard,__llvm_kcfi_dispatch_22222222
        .globl __llvm_kcfi_dispatch_22222222
        .p2align 4
__llvm_kcfi_dispatch_22222222:
        cmpl $0x22222222, -4(%rax)
        jne __llvm_kcfi_mismatch_22222222
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

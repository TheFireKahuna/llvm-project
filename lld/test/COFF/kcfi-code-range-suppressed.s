# REQUIRES: x86

## An export-suppressed function, one that the guard function table lists only
## because the image exports it, keeps its KCFI type, but Control Flow Guard
## accepts a pointer to it only once GetProcAddress returns it or an importer's
## import grants it. In a sealed image its chunk follows the other chunks of
## the plain .text group and the code range ends before it, so that a match on
## it never skips the guard function.

# RUN: split-file %s %t.dir
# RUN: llvm-mc -triple x86_64-windows-msvc %t.dir/dll.s -filetype=obj -o %t.dll.obj
# RUN: llvm-mc -triple x86_64-windows-msvc %t.dir/twin.s -filetype=obj -o %t.twin.obj

## exported moves after entry, and the range [0x180001000, 0x180001030) ends
## at its chunk.
# RUN: lld-link %t.dll.obj -dll -noentry -export:exported -guard:cf \
# RUN:   -debug:symtab -out:%t.dll
# RUN: llvm-objdump -t %t.dll | FileCheck %s --check-prefix=MOVED
# RUN: llvm-objdump -s --section=.data %t.dll | FileCheck %s --check-prefix=RANGE
# MOVED-DAG: 0x00000040 exported
# MOVED-DAG: 0x00000010 taken
# MOVED-DAG: 0x00000020 entry
# MOVED-DAG: 0x00000030 __llvm_code_end
# RANGE: {{^ [0-9a-f]+}} 00100080 01000000 30100080 01000000

## A function that /guardsym suppresses is kept out of the range too: the
## guard function accepts it only once the process makes it valid.
# RUN: lld-link %t.dll.obj -dll -noentry -guardsym:taken,S -guard:cf \
# RUN:   -debug:symtab -out:%t.sym.dll
# RUN: llvm-objdump -t %t.sym.dll | FileCheck %s --check-prefix=SYM
# SYM-DAG: 0x00000040 taken
# SYM-DAG: 0x00000030 __llvm_code_end

## A function the image also takes the address of is not suppressed, and the
## range covers the section.
# RUN: lld-link %t.dll.obj -dll -noentry -export:taken -guard:cf \
# RUN:   -debug:symtab -out:%t.taken.dll
# RUN: llvm-objdump -t %t.taken.dll | FileCheck %s --check-prefix=STAYS
# STAYS-DAG: 0x00000010 exported
# STAYS-DAG: 0x00000030 taken
# STAYS-DAG: 0x00000046 __llvm_code_end

## A chunk that /order places keeps its place, and the range ends before it,
## here at its start, so the range is empty.
# RUN: echo exported > %t.order
# RUN: lld-link %t.dll.obj -dll -noentry -export:exported -guard:cf \
# RUN:   -debug:symtab -order:@%t.order -out:%t.order.dll
# RUN: llvm-objdump -t %t.order.dll | FileCheck %s --check-prefix=ORDER
# ORDER-DAG: (sec  1){{.*}} 0x00000010 exported
# ORDER-DAG: (sec  2){{.*}} 0x00000000 __llvm_code_end

## Identical code folding makes twin's chunk taken's, which the table lists, so
## it stays inside the range.
# RUN: lld-link %t.dll.obj %t.twin.obj -dll -noentry -export:twin -opt:icf \
# RUN:   -guard:cf -debug:symtab -out:%t.icf.dll
# RUN: llvm-objdump -t %t.icf.dll | FileCheck %s --check-prefix=ICF
# ICF-DAG: 0x00000030 twin
# ICF-DAG: 0x00000030 taken
# ICF-DAG: 0x00000048 __llvm_code_end

#--- dll.s
  .linktypeprefixes
        .globl @feat.00
@feat.00 = 0x800

        .def exported; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,exported
        .p2align 4
        .fill 4, 1, 0x90
__cfi_exported:
        nopl 0x71c5a06(%rax)
        movl $0x11111111, %eax
        .globl exported
exported:
        retq

        .def taken; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,taken
        .p2align 4
        .fill 4, 1, 0x90
__cfi_taken:
        nopl 0x71c5a06(%rax)
        movl $0x11111111, %eax
        .globl taken
taken:
        movl $1, %eax
        retq

        .def entry; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,entry
        .p2align 4
        .globl entry
entry:
        movl $1, %eax
        retq

        .weak __llvm_code_start
__llvm_code_start = __llvm_code_empty
        .weak __llvm_code_end
__llvm_code_end = __llvm_code_empty
        .section .rdata,"dr",discard,__llvm_code_empty
        .globl __llvm_code_empty
__llvm_code_empty:
        .byte 0

        .data
        .quad __llvm_code_start
        .quad __llvm_code_end
        .quad taken

        .section .gfids$y,"dr"
        .symidx taken

#--- twin.s
  .linktypeprefixes
        .def twin; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,twin
        .p2align 4
        .fill 4, 1, 0x90
__cfi_twin:
        nopl 0x71c5a06(%rax)
        movl $0x11111111, %eax
        .globl twin
twin:
        movl $1, %eax
        retq

# REQUIRES: x86

## Control Flow Guard accepts every address in the 16-byte granule of an entry
## the guard function table lists off a 16-byte boundary. The image keeps the
## sealed KCFI prefix, with type 0, of a function whose entry would lie off a
## boundary once the prefix is left out, where that entry could then share
## such a granule: here, listed's chunk ends short of its granule's end, and
## small's entry would follow in that granule. An entry that stays on a
## boundary, aligned's, still leaves out its prefix. A chunk that itself holds
## a listed entry off a boundary, as holder's does, keeps its prefix.

# RUN: split-file %s %t.dir
# RUN: llvm-mc -triple x86_64-windows-msvc %t.dir/open.s -filetype=obj \
# RUN:   -o %t.open.obj
# RUN: lld-link %t.open.obj -guard:cf -entry:main -debug:symtab \
# RUN:   -out:%t.open.exe
# RUN: llvm-objdump -d %t.open.exe | FileCheck %s --check-prefix=OPEN
# RUN: llvm-mc -triple x86_64-windows-msvc %t.dir/holds.s -filetype=obj \
# RUN:   -o %t.holds.obj
# RUN: lld-link %t.holds.obj -guard:cf -entry:main -debug:symtab \
# RUN:   -out:%t.holds.exe
# RUN: llvm-objdump -d %t.holds.exe | FileCheck %s --check-prefix=HOLDS

## listed's chunk ends at 0x140001025; without its prefix, small's entry would
## follow at 0x140001028.
# OPEN:      <listed>:
# OPEN-NEXT:   140001024: c3 retq
# OPEN:      <__cfi_small>:
# OPEN-NEXT:   140001028: {{.*}} nopl 0x71c5a06(%rax)
# OPEN-NEXT:   movl $0x0, %eax
# OPEN-EMPTY:
# OPEN-NEXT: <small>:
# OPEN-NEXT:   140001034: c3 retq
# OPEN-NOT:  nopl
# OPEN:      <aligned>:
# OPEN-NEXT:   140001040: c3 retq

## Without its prefix, holder's chunk would start at 0x140001000, over main,
## and listed's granule would hold main's bytes.
# HOLDS:      <__cfi_holder>:
# HOLDS-NEXT:   140001010: {{.*}} nopl 0x71c5a06(%rax)
# HOLDS-NEXT:   movl $0x0, %eax
# HOLDS-EMPTY:
# HOLDS-NEXT: <holder>:

#--- open.s
  .linktypeprefixes
        .globl @feat.00
@feat.00 = 0x800

        .def main; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,main
        .p2align 4
        .globl main
main:
        callq small
        callq aligned
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        retq

        .def listed; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,listed
        .p2align 4
        int3
        int3
        int3
        int3
        .globl listed
listed:
        retq

        .def small; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,small
        .p2align 2
__cfi_small:
        nopl 0x71c5a06(%rax)
        movl $0x11111111, %eax
        .globl small
small:
        retq

        .def aligned; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,aligned
        .p2align 4
        .fill 4, 1, 0x90
__cfi_aligned:
        nopl 0x71c5a06(%rax)
        movl $0x22222222, %eax
        .globl aligned
aligned:
        retq

        .section .gfids$y,"dr"
        .symidx listed

#--- holds.s
  .linktypeprefixes
        .globl @feat.00
@feat.00 = 0x800

        .def main; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,main
        .p2align 4
        .globl main
main:
        callq holder
        retq

        .def holder; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,holder
        .p2align 4
__cfi_holder:
        nopl 0x71c5a06(%rax)
        movl $0x11111111, %eax
        .globl holder
holder:
        nop
        nop
        .globl listed
listed:
        .fill 17, 1, 0x90
        retq

        .section .gfids$y,"dr"
        .symidx listed

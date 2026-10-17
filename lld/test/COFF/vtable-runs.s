# REQUIRES: x86

## The vtable records lay out the vtables of a class and of the classes derived
## from it through primary bases as one run of equal slots, and the linker
## defines the range symbols a guard thunk and a checked downcast test: the
## class's own address point, the bytes its subtree's slots span, the stride
## less one, and the address that carries the stride's log2 for a target
## without relocatable immediates. A class whose vtable shares a chunk with
## another, as a merged global's do, is left out and gets an empty range, as
## does a class with no vtable laid out in a run.

# RUN: split-file %s %t
# RUN: llvm-mc -triple x86_64-unknown-windows-itanium %t/a.s -filetype=obj -o %t/a.obj
# RUN: lld-link %t/a.obj -entry:main -debug:symtab -opt:icf -out:%t/out.exe
# RUN: llvm-nm --numeric-sort %t/out.exe | FileCheck %s

## The run A -> B -> C: each vtable is 24 bytes, so with the address point 16 in
## the slot is 64 bytes. A's range spans all three slots, B's the last two,
## C's the last one. The first address point is 16 into a 64-byte line.
# CHECK-DAG: [[#%x,A:]] R _ZTV1A

## The start symbols are the classes' address points, 16 into each vtable, the
## slots 64 apart: A, B, C at A+16, A+80, A+144.
# CHECK-DAG: [[#A+16]] R __llvm_vtables$1A
# CHECK-DAG: [[#A+16]] R _ZTV1A.ap
# CHECK-DAG: [[#A+80]] R __llvm_vtables$1B
# CHECK-DAG: [[#A+144]] R __llvm_vtables$1C
# CHECK-DAG: [[#A+64]] R _ZTV1B
# CHECK-DAG: [[#A+128]] R _ZTV1C

## The end is one past the subtree's last slot: A spans 3 slots (end A+16+192),
## B spans 2 (A+80+128), C spans 1 (A+144+64).
# CHECK-DAG: [[#A+16+192]] R __llvm_vtables_end$1A
# CHECK-DAG: [[#A+80+128]] R __llvm_vtables_end$1B
# CHECK-DAG: [[#A+144+64]] R __llvm_vtables_end$1C

## The mask is a read-only word, the same for every class of the run.
# CHECK-DAG: [[#%x,MASK:]] R __llvm_vtables_mask$1A
# CHECK-DAG: [[#MASK]] R __llvm_vtables_mask$1B
# CHECK-DAG: [[#MASK]] R __llvm_vtables_mask$1C

## A merged global holds M and N, so neither heads a run; a class no record
## places has none either. Their range symbols are defined as an empty range at
## the image base rather than a run address (the link resolves them, and nm
## does not list an image-base synthetic), so no run symbol names them.
# CHECK-NOT: R __llvm_vtables$1M
# CHECK-NOT: R __llvm_vtables$1N
# CHECK-NOT: R __llvm_vtables$1Z

#--- a.s
  .text
  .globl main
main:
  leaq _ZTV1A(%rip), %rax
  leaq _ZTV1B(%rip), %rax
  leaq _ZTV1C(%rip), %rax
  leaq _ZTV1A.ap(%rip), %rax
  movq __llvm_vtables$1A(%rip), %rax
  movq __llvm_vtables$1B(%rip), %rax
  movq __llvm_vtables$1C(%rip), %rax
  movq __llvm_vtables_end$1A(%rip), %rax
  movq __llvm_vtables_end$1B(%rip), %rax
  movq __llvm_vtables_end$1C(%rip), %rax
  movq __llvm_vtables_mask$1A(%rip), %rax
  movq __llvm_vtables_mask$1B(%rip), %rax
  movq __llvm_vtables_mask$1C(%rip), %rax
  movq _ZTV1M(%rip), %rax
  movq _ZTV1N(%rip), %rax
  movq __llvm_vtables$1M(%rip), %rax
  movq __llvm_vtables_end$1M(%rip), %rax
  movq __llvm_vtables$1N(%rip), %rax
  movq __llvm_vtables$1Z(%rip), %rax
  retq

  .section .rdata,"dr",discard,_ZTV1A
  .globl _ZTV1A
  .p2align 3
_ZTV1A:
  .quad 0, 0
  .globl _ZTV1A.ap
_ZTV1A.ap:
  .quad 0
  .linkvtable _ZTV1A, 16, "1A", ""

  .section .rdata,"dr",discard,_ZTV1B
  .globl _ZTV1B
  .p2align 3
_ZTV1B:
  .quad 0, 0, 0
  .linkvtable _ZTV1B, 16, "1B", "1A"

  .section .rdata,"dr",discard,_ZTV1C
  .globl _ZTV1C
  .p2align 3
_ZTV1C:
  .quad 0, 0, 0
  .linkvtable _ZTV1C, 16, "1C", "1B"

## One chunk, two classes: a merged global, left out.
  .section .rdata,"dr",discard,_ZTV1M
  .globl _ZTV1M
  .p2align 3
_ZTV1M:
  .quad 0, 0, 0
  .globl _ZTV1N
_ZTV1N:
  .quad 0, 0, 0
  .linkvtable _ZTV1M, 16, "1M", ""
  .linkvtable _ZTV1N, 16, "1N", ""

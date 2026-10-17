# REQUIRES: x86

## A vtable whose entry is an in-place import slot is held back into the import
## address table span, so it cannot move into a run; its class takes the slow
## path with an empty range. A class whose vtable holds no slot is laid out in
## a run as usual, in the same link.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium main.s -o main.obj
# RUN: lld-link -def:lib.def -out:lib.lib -machine:x64
# RUN: lld-link -import-slots -entry:main -subsystem:console main.obj lib.lib \
# RUN:   -debug:symtab -out:out.exe
# RUN: llvm-nm --numeric-sort out.exe | FileCheck %s

## B holds no import slot: its run is placed, so its vtable sits at the run
## start less 16 (the address point at offset 16) and the range spans one slot.
# CHECK-DAG: [[#%x,B:]] R _ZTV1B
# CHECK-DAG: [[#B+16]] R __llvm_vtables$1B
# CHECK-DAG: [[#B+16+64]] R __llvm_vtables_end$1B

## A holds an import slot and is held back near the import address table, so its
## run is abandoned: its range symbols are an empty range at the image base,
## which nm does not list, rather than a run address.
# CHECK-NOT: R __llvm_vtables$1A

#--- lib.def
LIBRARY lib.dll
EXPORTS
  vf

#--- main.s
  .text
  .globl main
main:
  leaq _ZTV1B(%rip), %rax
  movq __llvm_vtables$1A(%rip), %rax
  movq __llvm_vtables_end$1A(%rip), %rax
  movq __llvm_vtables$1B(%rip), %rax
  movq __llvm_vtables_end$1B(%rip), %rax
  retq

  .section .rdata,"dr",discard,_ZTV1A
  .globl _ZTV1A
  .p2align 3
_ZTV1A:
  .quad 0
  .quad _ZTI1A
  .quad vf
  .linkvtable _ZTV1A, 16, "1A", ""

  .section .rdata,"dr",discard,_ZTV1B
  .globl _ZTV1B
  .p2align 3
_ZTV1B:
  .quad 0
  .quad _ZTI1B
  .quad g
  .linkvtable _ZTV1B, 16, "1B", ""

  .section .rdata,"dr",discard,_ZTI1A
  .globl _ZTI1A
_ZTI1A:
  .quad 0
  .section .rdata,"dr",discard,_ZTI1B
  .globl _ZTI1B
_ZTI1B:
  .quad 0

  .text
  .globl g
g:
  retq

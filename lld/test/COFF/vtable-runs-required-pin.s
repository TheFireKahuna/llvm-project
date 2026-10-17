# REQUIRES: x86

## A vtable that a required pin places, as a tagged vtable is, sits at its pin
## and cannot take a slot of a run, so the run that would hold it is abandoned
## and its classes take the slow path with an empty range. The pin is still
## honoured. A run of vtables without such a pin is laid out as usual.

# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium %s -o %t.obj
# RUN: lld-link -entry:main -subsystem:console %t.obj -debug:symtab -out:%t.exe
# RUN: llvm-nm --numeric-sort %t.exe | FileCheck %s

## B's run is placed.
# CHECK-DAG: [[#%x,B:]] R _ZTV1B
# CHECK-DAG: [[#B+16]] R __llvm_vtables$1B

## T's vtable keeps its required pin: its address point, 16 bytes in, is 424
## modulo a page. Its run is abandoned.
# CHECK-DAG: {{[0-9a-f]*}}198 R _ZTV1T
# CHECK-NOT: R __llvm_vtables$1T

  .text
  .globl main
main:
  movq __llvm_vtables$1B(%rip), %rax
  movq __llvm_vtables$1T(%rip), %rax
  movq __llvm_vtables_end$1T(%rip), %rax
  retq

  .section .rdata,"dr",discard,_ZTV1B
  .globl _ZTV1B
  .p2align 3
_ZTV1B:
  .quad 0
  .quad 0
  .quad main
  .linkvtable _ZTV1B, 16, "1B", ""

  .section .rdata,"dr",discard,_ZTV1T
  .globl _ZTV1T
  .p2align 3
_ZTV1T:
  .quad 0
  .quad 0
  .quad main
  .linkpin _ZTV1T, 12, 408, required
  .linkvtable _ZTV1T, 16, "1T", ""

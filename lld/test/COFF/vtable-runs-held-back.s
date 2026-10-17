# REQUIRES: x86

## A vtable whose entry is an in-place import slot is held back into the import
## address table directory, and its run is laid out there, the address point
## 16 bytes into a line as anywhere else. A class whose vtable holds no slot is
## laid out in a run in plain read-only data, in the same link. A held-back
## vtable whose slots run on into the next chunk cannot leave it, so its run is
## left out, with an empty range.

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

## A holds an import slot and is held back into the directory, where its run
## is placed: its slot is the first entry of the address table.
# CHECK-DAG: [[#%x,A:]] R _ZTV1A
# CHECK-DAG: [[#A+16]] R __llvm_vtables$1A
# CHECK-DAG: [[#A+16+64]] R __llvm_vtables_end$1A
# RUN: llvm-readobj --coff-imports out.exe | FileCheck %s --check-prefix=IAT
# IAT: ImportAddressTableRVA: 0x2010

## C's last entry is a slot whose run continues into the chunk after it, so
## C's run is an empty range at the image base, which nm does not list.
# RUN: llvm-nm out.exe | FileCheck %s --check-prefix=CONT
# CONT-NOT: __llvm_vtables$1C

#--- lib.def
LIBRARY lib.dll
EXPORTS
  vf
  vf2

#--- main.s
  .text
  .globl main
main:
  leaq _ZTV1B(%rip), %rax
  movq __llvm_vtables$1A(%rip), %rax
  movq __llvm_vtables_end$1A(%rip), %rax
  movq __llvm_vtables$1B(%rip), %rax
  movq __llvm_vtables_end$1B(%rip), %rax
  movq __llvm_vtables$1C(%rip), %rax
  leaq next(%rip), %rax
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

  .section .rdata,"dr",discard,_ZTV1C
  .globl _ZTV1C
  .p2align 3
_ZTV1C:
  .quad 0
  .quad _ZTI1A
  .quad vf
  .linkvtable _ZTV1C, 16, "1C", ""
  .section .rdata,"dr",discard,next
  .globl next
  .p2align 3
next:
  .quad vf2

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

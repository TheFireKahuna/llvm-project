# REQUIRES: x86

## A tagged vtable, which a required pin places, carries a record naming its
## primary base but takes no slot of the base's run. The layout offers it the
## run's gaps first, where its pin falls inside one, so that a hierarchy's
## vtables share their pages. Under /verbose the linker lists tagged vtables
## of one hierarchy that share a tag.

# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium %s -o %t.obj
# RUN: lld-link -entry:main -subsystem:console %t.obj -debug:symtab \
# RUN:   -out:%t.exe -verbose 2>&1 | FileCheck %s --check-prefix=LOG
# RUN: llvm-nm --numeric-sort %t.exe | FileCheck %s

## B's vtable fills most of a 4096-byte slot; T sits at its pin in the slot's
## tail, inside the run, and the run still holds B alone. Other read-only data
## fills the rest of the gap, in place of padding; U, whose pin T took, starts
## the next page's.
# CHECK:      [[#%x,B:]] R _ZTV1B
# CHECK-NEXT: [[#B+16]] R __llvm_vtables$1B
# CHECK-NEXT: [[#B+3016]] R filler
# CHECK-NEXT: [[#B+3624]] R _ZTV1T
# CHECK-NEXT: [[#B+16+4096]] R __llvm_vtables_end$1B

# LOG: tagged vtables of 1T and 1U, derived from 1B, share the tag 3640

  .text
  .globl main
main:
  movq __llvm_vtables$1B(%rip), %rax
  movq __llvm_vtables_end$1B(%rip), %rax
  leaq _ZTV1T(%rip), %rax
  leaq _ZTV1U(%rip), %rax
  leaq filler(%rip), %rax
  retq

  .section .rdata,"dr"
  .globl filler
  .p2align 3
filler:
  .fill 64, 1, 1

  .section .rdata,"dr",discard,_ZTV1B
  .globl _ZTV1B
  .p2align 6
_ZTV1B:
  .quad 0
  .quad 0
  .fill 375, 8, 0
  .linkvtable _ZTV1B, 16, "1B", ""

  .section .rdata,"dr",discard,_ZTV1T
  .globl _ZTV1T
  .p2align 3
_ZTV1T:
  .quad 0
  .quad 0
  .quad 0
  .linkpin _ZTV1T, 12, 3624, required
  .linkvtable _ZTV1T, 16, "1T", "1B"

  .section .rdata,"dr",discard,_ZTV1U
  .globl _ZTV1U
  .p2align 3
_ZTV1U:
  .quad 0
  .quad 0
  .quad 0
  .linkpin _ZTV1U, 12, 3624, required
  .linkvtable _ZTV1U, 16, "1U", "1B"

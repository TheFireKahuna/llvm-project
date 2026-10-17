# REQUIRES: x86

## A pin naming a section's symbol, as the assembler writes for a private
## label, pins the section. When COMDAT selection discards the section, the
## pin applies to nothing and the kept copy's own pin places it.

# RUN: split-file %s %t
# RUN: llvm-mc -triple x86_64-unknown-windows-itanium %t/a.s -filetype=obj -o %t/a.obj
# RUN: llvm-mc -triple x86_64-unknown-windows-itanium %t/b.s -filetype=obj -o %t/b.obj
# RUN: lld-link %t/a.obj %t/b.obj -entry:main -debug:symtab -out:%t/out.exe
# RUN: llvm-nm %t/out.exe | FileCheck %s

## The kept copy is a.obj's, at 40 modulo 64.
# CHECK: 140002028 R vt

#--- a.s
  .text
  .globl main
main:
  leaq vt(%rip), %rax
  retq

  .section .rdata,"dr",discard,vt
  .globl vt
  .p2align 3
vt:
.Lvt:
  .quad 0
  .linkpin .Lvt, 6, 40, required

#--- b.s
  .section .rdata,"dr",discard,vt
  .globl vt
  .p2align 3
vt:
.Lvt:
  .quad 0
  .linkpin .Lvt, 6, 16, required

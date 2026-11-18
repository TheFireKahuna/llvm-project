# REQUIRES: x86

## A local import pointer keeps its symbol alive, and has no base relocation
## when its symbol is absolute. An object that refers to it only from data,
## such as a catch-type entry, which a compiler made, is not reported as
## importing a locally defined symbol; one whose code refers to it is.

# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %s -o %t.obj
# RUN: lld-link -entry:main -out:%t.exe %t.obj -opt:ref 2>&1 \
# RUN:   | FileCheck --check-prefix=WARN %s
# RUN: llvm-objdump -s -j .rdata %t.exe | FileCheck %s
# RUN: llvm-readobj --coff-basereloc %t.exe | FileCheck --check-prefix=RELOC %s

# WARN:     warning: {{.*}}.obj: locally defined symbol imported: f (defined in {{.*}}.obj) [LNK4217]
# WARN-NOT: locally defined symbol imported

## The pointers hold abs, and f, whose COMDAT only its pointer refers to.
# CHECK:      140002000 34120000 00000000 10100040 01000000
# RELOC:      Type: DIR64
# RELOC-NEXT: Address: 0x2008
# RELOC-NOT:  Type: DIR64

  .globl main
main:
  movq __imp_f(%rip), %rax
  retq

  .data
  .long __imp_abs-.

  .section .text,"xr",discard,f
  .p2align 4
  .globl f
f:
  retq

  .globl abs
  .set abs, 0x1234

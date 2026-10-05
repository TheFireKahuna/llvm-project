# REQUIRES: aarch64

## Pins are honoured on AArch64 as on x86-64, with and without -import-slots.

# RUN: llvm-mc -triple aarch64-unknown-windows-itanium %s -filetype=obj -o %t.obj
# RUN: lld-link %t.obj -entry:main -debug:symtab -out:%t.exe
# RUN: llvm-nm -n %t.exe | FileCheck %s --check-prefix=PLAIN
# RUN: lld-link %t.obj -entry:main -debug:symtab -import-slots -out:%t.slots.exe
# RUN: llvm-nm -n %t.slots.exe | FileCheck %s --check-prefix=PLAIN

# PLAIN:      140002000 r s8
# PLAIN-NEXT: 140002040 r a64
# PLAIN-NEXT: 140002fc8 R tagged

  .text
  .globl main
main:
  adrp x0, s8
  adrp x0, a64
  adrp x0, tagged
  ret

  .section .rdata,"dr",one_only,s8
  .p2align 3
s8:
  .quad 0

  .section .rdata,"dr",one_only,a64
  .p2align 6
a64:
  .quad 0, 0

  .section .rdata,"dr",one_only,tagged
  .globl tagged
  .p2align 3
tagged:
  .quad 0, 0, 0
  .linkpin tagged, 12, 4040, required

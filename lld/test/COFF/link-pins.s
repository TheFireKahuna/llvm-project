# REQUIRES: x86

## A pin of the link-only records asks for a symbol's address modulo a power
## of two. The linker pads before the pinned section until its start takes
## the residue the pin gives, less the symbol's offset in it, whether or not
## -import-slots is given. A pin names a symbol, so a pin that a discarded
## COMDAT copy carries applies to the kept copy; a pin naming a section's
## symbol pins the section. Pinned sections are not folded by ICF, and the
## linker defines __llvm_link_pins_v1 for objects that need pins honoured.

# RUN: split-file %s %t
# RUN: llvm-mc -triple x86_64-unknown-windows-itanium %t/a.s -filetype=obj -o %t/a.obj
# RUN: llvm-mc -triple x86_64-unknown-windows-itanium %t/b.s -filetype=obj -o %t/b.obj
# RUN: lld-link %t/a.obj %t/b.obj -entry:main -debug:symtab -opt:icf \
# RUN:   -include:__llvm_link_pins_v1 -out:%t/out.exe
# RUN: llvm-nm -n %t/out.exe | FileCheck %s
# RUN: lld-link %t/a.obj %t/b.obj -entry:main -debug:symtab -opt:icf \
# RUN:   -import-slots -out:%t/slots.exe
# RUN: llvm-nm -n %t/slots.exe | FileCheck --check-prefix=SLOTS %s

## _ZTV1D.ap: required, 16 into its section, at 4072 modulo 4096.
## _ZTV1C: a.obj's copy is kept, pinned by b.obj's at 24 modulo 64.
## _ZTV1E: required at 0 modulo 64; _ZTV1F, identical to it, is not folded.
## .Linner: 8 into its section, at 40 modulo 64.
# CHECK:      140002018 R _ZTV1C
# CHECK-NEXT: 140002fd8 r _ZTV1D
# CHECK-NEXT: 140002fe8 R _ZTV1D.ap
# CHECK-NEXT: 140003000 r _ZTV1E
# CHECK-NEXT: 140003010 r _ZTV1F
# CHECK-NEXT: 140003020 r inner_start

## -import-slots also packs .rdata; the pins hold.
# SLOTS:      140002000 r _ZTV1E
# SLOTS-NEXT: 140002018 R _ZTV1C
# SLOTS-NEXT: 140002030 r _ZTV1F
# SLOTS-NEXT: 140002fd8 r _ZTV1D
# SLOTS-NEXT: 140002fe8 R _ZTV1D.ap
# SLOTS-NEXT: 140003020 r inner_start

#--- a.s
  .text
  .globl main
main:
  leaq _ZTV1D(%rip), %rax
  leaq _ZTV1C(%rip), %rax
  leaq _ZTV1E(%rip), %rax
  leaq _ZTV1F(%rip), %rax
  leaq inner_start(%rip), %rax
  retq

  .section .rdata,"dr",discard,_ZTV1C
  .globl _ZTV1C
  .p2align 3
_ZTV1C:
  .quad 1, 2, 3

  .section .rdata,"dr",one_only,_ZTV1D
  .p2align 3
_ZTV1D:
  .quad 0, 0
  .globl _ZTV1D.ap
_ZTV1D.ap:
  .quad 4
  .linkpin _ZTV1D.ap, 12, 4072, required

  .section .rdata,"dr",discard,_ZTV1E
  .p2align 3
_ZTV1E:
  .quad 7, 7
  .linkpin _ZTV1E, 6, 0, required

  .section .rdata,"dr",discard,_ZTV1F
  .p2align 3
_ZTV1F:
  .quad 7, 7

  .section .rdata$inner,"dr"
  .p2align 3
inner_start:
  .quad 0
.Linner:
  .quad 0
  .linkpin .Linner, 6, 40

#--- b.s
  .section .rdata,"dr",discard,_ZTV1C
  .globl _ZTV1C
  .p2align 3
_ZTV1C:
  .quad 1, 2, 3
  .linkpin _ZTV1C, 6, 24

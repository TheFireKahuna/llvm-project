# REQUIRES: aarch64

## On ARM64, an object whose compiler reaches through an import pointer every
## symbol it cannot place in the image, which says so by carrying link-only
## records, is reported only for the pointers its references still read once
## the adrp and ldr loads of a pointer are rewritten.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium defs.s -o defs.obj
# RUN: lld-link -entry:main -subsystem:console -out:a.exe main.obj defs.obj \
# RUN:   2>&1 | FileCheck %s

# CHECK-NOT: locally defined symbol imported: f
# CHECK:     warning: main.obj: locally defined symbol imported: g (defined in defs.obj) [LNK4217]
# CHECK-NOT: locally defined symbol imported: f

#--- main.s
  .text
  .globl main
main:
  adrp x0, __imp_f
  ldr x0, [x0, :lo12:__imp_f]
  adrp x1, __imp_g
  ldr x1, [x1, :lo12:__imp_g]
  adrp x2, __imp_g
  add x2, x2, :lo12:__imp_g
  ret

#--- defs.s
  .text
  .globl f, g
  .p2align 2
g:
  ret
f:
  ret

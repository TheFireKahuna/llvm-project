# REQUIRES: aarch64

## On ARM64, where a local import pointer of data is bypassed, so that its adrp
## computes the data's page and its ldr becomes an add of the data's offset in
## the page, the accesses described with the ldr take that offset as well,
## and the ldr becomes a nop, if every one of them fits its field and is
## aligned to its size; a pair of loads or stores has a signed 7-bit field. An add described with the ldr is folded into it, the
## add becoming a nop, if the sum fits. Otherwise both are left as they are.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium defs.s -o defs.obj
# RUN: lld-link -entry:main -subsystem:console -out:a.exe main.obj defs.obj \
# RUN:   -verbose 2>&1 | FileCheck --check-prefix=LOG %s
# RUN: llvm-objdump -d --no-show-raw-insn a.exe | FileCheck %s

# LOG: Fused accesses and adds: 6 of 6 direct

# CHECK:      <.text>:
# CHECK-NEXT: adrp x8, 0x140002000
# CHECK-NEXT: nop
# CHECK-NEXT: ldr w0, [x8, #0x40]
# CHECK-NEXT: str w0, [x8, #0x48]
# CHECK-NEXT: ldp x4, x5, [x8, #0x40]
# CHECK-NEXT: adrp x9, 0x140002000
# CHECK-NEXT: add x1, x9, #0x60
# CHECK-NEXT: nop
# CHECK-NEXT: adrp x10, 0x140002000
# CHECK-NEXT: add x10, x10, #0x60
# CHECK-NEXT: add x2, x10, #0xff0
# CHECK-NEXT: adrp x11, 0x140002000
# CHECK-NEXT: add x11, x11, #0x65
# CHECK-NEXT: ldr x3, [x11]
# CHECK-NEXT: ret

#--- main.s
  .text
  .globl main
  .p2align 2
main:
  adrp x8, __imp_d
.Lf0:
  .linkfusedread
  ldr x8, [x8, :lo12:__imp_d]
  .linkfusedaccess .Lf0, x8
  ldr w0, [x8]
  .linkfusedaccess .Lf0, x8
  str w0, [x8, #8]
  .linkfusedaccess .Lf0, x8
  ldp x4, x5, [x8]
  adrp x9, __imp_e
.Lf1:
  ldr x9, [x9, :lo12:__imp_e]
  .linkfusedadd .Lf1, x9
  add x1, x9, #16
## The sum, 0x60 + 4080, does not fit 12 bits.
  adrp x10, __imp_big
.Lf2:
  ldr x10, [x10, :lo12:__imp_big]
  .linkfusedadd .Lf2, x10
  add x2, x10, #4080
## odd is not aligned to the 8 bytes the access reads.
  adrp x11, __imp_odd
.Lf3:
  .linkfusedread
  ldr x11, [x11, :lo12:__imp_odd]
  .linkfusedaccess .Lf3, x11
  ldr x3, [x11]
  ret

#--- defs.s
  .data
  .globl d, e, big, odd
  .zero 64
d:
  .zero 16
e:
  .zero 16
big:
  .zero 5
odd:
  .zero 8

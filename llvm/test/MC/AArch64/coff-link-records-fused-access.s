// On ARM64, .linkfusedaccess and .linkfusedadd describe the accesses and adds
// through the register that ldr of an import pointer of data fills, as on
// x86-64; an access only at an immediate offset from the register, a load,
// store or prefetch at an unsigned one or a pair at a signed one, which does
// not store the register itself, and an add only of an immediate. The linker
// finds every other fact in the instruction, so an access's fact holds only
// the load's site.

// RUN: llvm-mc -triple aarch64-unknown-windows-itanium -filetype=obj %s -o %t.o
// RUN: llvm-objdump -s -j .llvm_link_records %t.o | FileCheck %s

// A group of kind 2 and 26 bytes: .text with 6 sites.
// - 4b 02 0600: the load at 0x4, which only accesses read (kind 6).
// - 48 02 0300: the ldr at 0x8, an access (kind 3) through site 0's register.
// - 48 02 0300: the stp at 0xc, a pair.
// - 8301: the load at 0x14.
// - 48 06 0463 05ffff01: the sub at 0x18, an add (kind 4) of site 3's
//   register into x3 (3), with the constant -16384 (kind 5).
// - 8301: the load at 0x20, whose register its access stores, which is not
//   described, nor so the load as one only accesses read.
// CHECK:      4c4c5243 010a021a 00064b02 06004802
// CHECK-NEXT: 03004802 03008301 48060463 05ffff01
// CHECK-NEXT: 8301

  .text
  .globl f
  .p2align 2
f:
  adrp x8, __imp_d
.Lf0:
  .linkfusedread
  ldr x8, [x8, :lo12:__imp_d]
  .linkfusedaccess .Lf0, x8
  ldr w0, [x8, #4]
  .linkfusedaccess .Lf0, x8
  stp x1, x2, [x8, #16]
  adrp x9, __imp_e
.Lf1:
  ldr x9, [x9, :lo12:__imp_e]
  .linkfusedadd .Lf1, x9
  sub x3, x9, #4, lsl #12
  adrp x10, __imp_g
.Lf2:
  .linkfusedread
  ldr x10, [x10, :lo12:__imp_g]
  .linkfusedaccess .Lf2, x10
  str x10, [x10]
  ret

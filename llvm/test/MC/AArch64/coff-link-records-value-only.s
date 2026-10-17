// .linkvalueonly says that the next instruction, the ldr of an import pointer
// of data, fills a register that only reads of memory read, as their base, and
// names the adrp that gives it the pointer's page. It becomes a fact of kind 7
// on the ldr's site, whose value is the distance in instructions back to the
// adrp. An adrp in another section, or after the ldr, drops the fact.

// RUN: llvm-mc -triple aarch64-unknown-windows-itanium -filetype=obj %s -o %t.o
// RUN: llvm-objdump -s -j .llvm_link_records %t.o | FileCheck %s
// RUN: llvm-mc -triple aarch64-unknown-windows-itanium %s | \
// RUN:   FileCheck --check-prefix=ASM %s

// A group of kind 2 and 15 bytes: .text with 4 sites.
// CHECK:      Contents of section .llvm_link_records:
// CHECK-NEXT: 0000 4c4c5243 0102020f 00044b02 07018b02
// CHECK-NEXT: 0010 02070283 018301
// CHECK-EMPTY:

// ASM:      .linkvalueonly .Lp0
// ASM-NEXT: ldr x8, [x8, :lo12:__imp_v]

  .text
  .globl f
f:
// 4b 02 0701: the ldr at 0x4, one instruction after its adrp.
.Lp0:
  adrp x8, __imp_v
  .linkvalueonly .Lp0
  ldr x8, [x8, :lo12:__imp_v]
  ldr w0, [x8, x0, lsl #2]
// 8b02 02 0702: the ldr at 0x14, two after its adrp.
.Lp1:
  adrp x9, __imp_v
  ldr w1, [x8]
  .linkvalueonly .Lp1
  ldr x9, [x9, :lo12:__imp_v]
  ldr w1, [x9]
// 8301: the ldr at 0x1c, whose adrp follows it.
  .linkvalueonly .Lp2
  ldr x10, [x10, :lo12:__imp_v]
.Lp2:
  adrp x10, __imp_v
// 8301: the ldr at 0x24, whose adrp is in another section.
  .linkvalueonly .Lp3
  ldr x11, [x11, :lo12:__imp_v]
  ret

  .section .text$cold,"xr"
.Lp3:
  adrp x11, __imp_v

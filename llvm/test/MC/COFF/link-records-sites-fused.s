// .linkfusedcall says that the next instruction, a call through a register
// after segment overrides or a jump through one before int3, goes through
// the register a load filled with a pointer, in as many bytes as the same
// transfer through the pointer in memory; and .linkfusedread that only such
// calls read the register the next instruction, a load, fills. A call has no
// relocation, and becomes a site of its own; a load's fact becomes data of
// its site.

// RUN: llvm-mc -triple x86_64-unknown-windows-itanium -filetype=obj %s -o %t.o
// RUN: llvm-objdump -s -j .llvm_link_records %t.o | FileCheck %s
// RUN: llvm-mc -triple x86_64-unknown-windows-itanium %s | \
// RUN:   FileCheck --check-prefix=ASM %s
// RUN: not llvm-mc -triple x86_64-unknown-windows-itanium -filetype=obj \
// RUN:   -defsym=ERR=1 %s -o /dev/null 2>&1 | FileCheck --check-prefix=ERR %s

// A group of kind 2 and 41 bytes: .text's symbol (index 0) with 11 sites. A
// site with data, bit 3, is followed by its size and the facts, each a kind
// and a value:
// - 3b 02 0204: a load at 0x3, read only by fused calls (kind 2); xorl, 2
//   bytes and no transfer, follows it (2 << 1 | 0).
// - 68 02 0106: a call at 0x9, of form 0, through rsi (6), which site 0
//   filled (0 << 6 | 0 << 5 | 6).
// - 9b01 02 0200: a fused read at 0x12, followed by another, so no
//   instruction carries its bytes.
// - 7b 02 0207: a fused read at 0x19, followed by testq, 3 bytes, which may
//   fuse with a branch (3 << 1 | 1).
// - 78 03 01aa01: a jump at 0x20 through r10, which site 2 filled
//   (2 << 6 | 1 << 5 | 10).
// - 78 03 01c703: a call at 0x27 through rdi, which site 7, after it,
//   filled (7 << 6 | 0 << 5 | 7).
// - 68 03 018805: a call at 0x2d through r8, after REX.B, which site 10
//   filled (10 << 6 | 0 << 5 | 8).
// - 9301: a load at 0x36 with no data, read by other instructions too.
// - 73: a load at 0x3d, whose call is not in the form described, so neither
//   is described.
// - c301: a load at 0x49, whose jump int3 does not follow, likewise.
// - b301: the load at 0x54 that the call through r8 reads.
// CHECK:      Contents of section .llvm_link_records:
// CHECK-NEXT: 0000 4c4c5243 01070229 000b3b02 02046802
// CHECK-NEXT: 0010 01069b01 0202007b 02020778 0301aa01
// CHECK-NEXT: 0020 780301c7 03680301 88059301 73c301b3
// CHECK-NEXT: 0030 01
// CHECK-EMPTY:

// ASM:      .Lr0:
// ASM-NEXT:   .linkfusedread
// ASM-NEXT:   movq __imp_g(%rip), %rsi
// ASM:        .linkfusedcall .Lr0, %rsi
// ASM-NEXT:   cs
// ASM:        callq *%rsi

// ERR: error: fused call reads the load at '.Lnone', which is not defined

.ifndef ERR
  .text
  .globl f
f:
.Lr0:
  .linkfusedread
  movq __imp_g(%rip), %rsi     // 0x0
  xorl %eax, %eax
  .linkfusedcall .Lr0, %rsi
  cs; cs; cs; cs               // 0x9
  callq *%rsi
.Lr1:
  .linkfusedread
  movq __imp_h(%rip), %r10     // 0xf
.Lr2:
  .linkfusedread
  movq __imp_k(%rip), %rdi     // 0x16
  testq %rax, %rax
  .linkfusedcall .Lr1, %r10
  rex64 jmpq *%r10             // 0x20
  int3; int3; int3; int3
  .linkfusedcall .Lr3, %rdi
  cs; cs; cs; cs               // 0x27
  callq *%rdi
  .linkfusedcall .Lr6, %r8
  cs; cs; cs                   // 0x2d
  callq *%r8
.Lr3:
  movq __imp_k(%rip), %rdi     // 0x33
  // Three overrides make a call through rax five bytes.
.Lr4:
  .linkfusedread
  movq __imp_g(%rip), %rax     // 0x3a
  .linkfusedcall .Lr4, %rax
  cs; cs; cs
  callq *%rax
.Lr5:
  .linkfusedread
  movq __imp_h(%rip), %rax     // 0x46
  .linkfusedcall .Lr5, %rax
  rex64 jmpq *%rax
  nop                          // not int3
.Lr6:
  movq __imp_g(%rip), %r8      // 0x51
.else
  .text
  .linkfusedcall .Lnone, %rax
  callq *%rax
.endif

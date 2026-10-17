// A reference through a segment register or a 32-bit address is a site of no
// form, since a linker that rewrites a call, jump or load keeps the
// instruction's prefixes, which would then apply to the new instruction.

// RUN: llvm-mc -triple x86_64-unknown-windows-itanium -filetype=obj %s -o %t.o
// RUN: llvm-objdump -s -j .llvm_link_records %t.o | FileCheck %s

// One group of kind 2 and 10 bytes: .text's symbol (index 0) with 6 sites,
// each of form 0 (other).
// CHECK:      Contents of section .llvm_link_records:
// CHECK-NEXT: 0000 4c4c5243 0103020a 00064070 70800170
// CHECK-NEXT: 0010 8001
// CHECK-EMPTY:

  .text
  .globl f
f:
  movq %fs:__imp_g(%rip), %rax  // 0x4
  callq *%gs:__imp_g(%rip)      // 0xb
  jmpq *%fs:__imp_g(%rip)       // 0x12
  movq __imp_g(%eip), %rax      // 0x1a
  callq *__imp_g(%eip)          // 0x21
  leaq g(%eip), %rax            // 0x29

// A pointer load with a REX2 prefix has a form of its own, since a linker that
// rewrites a load moves register bits within the prefix, and a REX2 prefix
// holds them elsewhere. Other forms are the same with either prefix.

// RUN: llvm-mc -triple x86_64-unknown-windows-itanium -mattr=+egpr \
// RUN:   -filetype=obj %s -o %t.o
// RUN: llvm-objdump -s -j .llvm_link_records %t.o | FileCheck %s

// One group of kind 2 and 8 bytes: .text's symbol (index 0) with 4 sites, of
// forms 3 (load) at 0x3, 6 (load with REX2) at 0xb, 4 (address) at 0x13 and
// 1 (call) at 0x19.
// CHECK:      Contents of section .llvm_link_records:
// CHECK-NEXT: 0000 4c4c5243 01030208 00043386 01840161
// CHECK-EMPTY:

  .text
  .globl f
f:
  movq __imp_g(%rip), %rax     // 0x3
  movq __imp_g(%rip), %r16     // 0xb
  leaq g(%rip), %r17           // 0x13
  callq *__imp_g(%rip)         // 0x19

// An lea of a symbol's address plus a constant is a site of its own form, 7,
// so that a linker that finds the symbol in another image can reach the
// address through the name that image exports for it; no other instruction
// with an offset says how it uses the address.

// RUN: llvm-mc -triple x86_64-unknown-windows-itanium -filetype=obj %s -o %t.o
// RUN: llvm-objdump -s -j .llvm_link_records %t.o | FileCheck %s

// "LLRC", version 1, capabilities 7, then one group of kind 2 and 5 bytes:
// .text's symbol (index 0) with 3 sites, each ULEB128 (delta << 4 | form):
// 0x3 address with an offset (7), 0xa address (4) and 0x11 other (0).
// CHECK:      Contents of section .llvm_link_records:
// CHECK-NEXT: 0000 4c4c5243 01070205 00033774 70
// CHECK-EMPTY:

  .text
  .globl f
f:
  leaq g+16(%rip), %rcx        // 0x3: address with an offset
  leaq g(%rip), %rcx           // 0xa: address
  movq g+8(%rip), %rax         // 0x11: other
  retq

// A jump that the assembler gave a 32-bit displacement in place of 8 bits,
// across code a linker may resize, is described as a jump field of 4 bytes,
// kind 0x42, which the linker may give 8 bits again; one that keeps 8 bits is
// a jump field of 1 byte, kind 0x12, which the linker may lengthen.

// RUN: llvm-mc -triple x86_64-unknown-windows-itanium -filetype=obj %s -o %t.o
// RUN: llvm-objdump -s -j .llvm_link_records %t.o | FileCheck %s

// A group of kind 2 and 4 bytes: the call through __imp_g at 0xa. Then a
// group of kind 30 and 10 bytes for .text in .text, with 2 entries:
// - 2a 42 8e01: the displacement at 0x2, of je with 4 bytes, to 0x90.
// - 5a 12 07: the displacement at 0x7, of je with 1 byte, to 0xe.
// CHECK:      Contents of section .llvm_link_records:
// CHECK-NEXT: 0000 4c4c5243 010f0204 0001a101 1e0a0000
// CHECK-NEXT: 0010 022a428e 015a1207
// CHECK-EMPTY:

  .text
  .globl f
f:
  je .Lfar
  je .Lnear
  callq *__imp_g(%rip)
.Lnear:
  .fill 130, 1, 0x90
.Lfar:
  retq

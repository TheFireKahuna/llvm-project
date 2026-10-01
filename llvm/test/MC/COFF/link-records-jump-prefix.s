// An indirect jump's site says how many prefix bytes precede its opcode,
// whether they are part of the jump or an instruction of their own, since a
// linker that makes it a direct jump rewrites it from its first byte. A jump
// is not described as one when the bytes before it may be a prefix but belong
// to no instruction, when a label separates it from a prefix before it, or
// after more than one prefix byte.

// RUN: llvm-mc -triple x86_64-unknown-windows-itanium -filetype=obj %s -o %t.o
// RUN: llvm-objdump -s -j .llvm_link_records %t.o | FileCheck %s

// "LLRC", version 1, the x86-64 sites capability, then one group of kind 2
// and 19 bytes: .text's symbol (index 0) with 12 sites, each ULEB128
// (delta << 4 | form), with forms 0 other, 1 call, 2 jump and 5 jump after
// one prefix.
// CHECK:      Contents of section .llvm_link_records:
// CHECK-NEXT: 0000 4c4c5243 01010213 000c2275 75757070
// CHECK-NEXT: 0010 800171e2 01800280 018001
// CHECK-EMPTY:

  .text
  jmpq *__imp_g(%rip)        // 0x2: jump, at the start of the section
  {rex} jmpq *__imp_g(%rip)  // 0x9: jump after one prefix, its own
  rex64 jmpq *__imp_g(%rip)  // 0x10: jump after one prefix, an instruction
  rex64
  jmpq *__imp_g(%rip)        // 0x17: jump after one prefix, on its own line
  rex64
.Llabel:
  jmpq *__imp_g(%rip)        // 0x1e: other, since .Llabel is reached without
                             // the prefix
  .byte 0x48
  jmpq *__imp_g(%rip)        // 0x25: other, after data
  rex64
  rex64 jmpq *__imp_g(%rip)  // 0x2d: other, after two prefixes
  rex64
  callq *__imp_g(%rip)       // 0x34: call, which any prefix may precede
  .p2align 4
  jmpq *__imp_g(%rip)        // 0x42: jump, after no-op padding
  nop
  .p2align 4, 0x48
  jmpq *__imp_g(%rip)        // 0x52: other, after padding with data
  rex64
.Llabel2:
  rex64 jmpq *__imp_g(%rip)  // 0x5a: other, since .Llabel2 splits the run
  .byte 0x2e
  rex64 jmpq *__imp_g(%rip)  // 0x62: other, after data before the prefix

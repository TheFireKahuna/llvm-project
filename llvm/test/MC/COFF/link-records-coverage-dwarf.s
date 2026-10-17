// A DWARF line advance across code a linker may resize is
// DW_LNS_fixed_advance_pc, whose two-byte operand the object's records
// describe for the linker to compute; one across nothing the linker resizes
// keeps its special opcode.

// RUN: llvm-mc -triple x86_64-unknown-windows-itanium -filetype=obj \
// RUN:   -dwarf-version=5 %s -o %t.o
// RUN: llvm-dwarfdump --debug-line -v %t.o | FileCheck %s
// RUN: llvm-objdump -s -j .llvm_link_records %t.o | \
// RUN:   FileCheck --check-prefix=RECORDS %s

// CHECK:      DW_LNE_set_address (0x0000000000000000)
// CHECK:      DW_LNS_copy
// CHECK:      DW_LNS_advance_line (2)
// CHECK:      DW_LNS_fixed_advance_pc (addr += 0x0006, op-index = 0)
// CHECK:      DW_LNS_copy
// CHECK:      address += 1,  line += 1,  op-index += 0
// CHECK:      DW_LNS_fixed_advance_pc (addr += 0x0007, op-index = 0)
// CHECK:      DW_LNE_end_sequence

// After the two call sites, a group of kind 30 and 14 bytes: .text's own,
// with no entry, then .debug_line's (symbol 8) for .text, with two
// differences in two bytes (20) and no constant: at 0x45 (d908), 0x6 - 0x0,
// and at 0x4a (5d, the same field), 0xe - 0x7, each place delta-coded.
// RECORDS:      Contents of section .llvm_link_records:
// RECORDS-NEXT: 0000 4c4c5243 01070204 00022171 1e0e0000
// RECORDS-NEXT: 0010 00080002 d9082006 005d0807
// RECORDS-EMPTY:

  .text
  .file 1 "a.c"
  .globl f
f:
  .loc 1 1
  callq *__imp_g(%rip)
  .loc 1 2
  nop
  .loc 1 3
  callq *__imp_g(%rip)
  retq

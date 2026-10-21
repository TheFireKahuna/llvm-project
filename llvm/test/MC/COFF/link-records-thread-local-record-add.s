// .linkfusedindex before add r64, r64 describes an add of the offset that a
// fused access of a thread-local variable's record loaded to another
// register, or of another register to it: kind 8 with bit 17 set, and bit 14
// set where the offset's register is the destination.

// RUN: llvm-mc -triple x86_64-unknown-windows-itanium -filetype=obj %s -o %t.o
// RUN: llvm-objdump -s -j .llvm_link_records %t.o | FileCheck %s

// A group of kind 2 and 0x19 bytes: .text with 4 sites.
// - 3b 02 0600: the record's load at 0x3, which only accesses read.
// - 48 05 038228 0900: an access at 0x7 of site 0, a load only indexes read.
// - 48 04 08828648: an index at 0xb of site 1 (1 << 20), an add (1 << 17)
//   of r9, its source, with REX at 0, ModRM at 2, 3 bytes.
// - 38 04 08828649: at 0xe, an add to r9, its destination (1 << 14).
// CHECK:      Contents of section .llvm_link_records:
// CHECK-NEXT: 0000 4c4c5243 010f0219 00043b02 06004805
// CHECK-NEXT: 0010 03822809 00480408 82864838 04088286
// CHECK-NEXT: 0020 491e0300 0000
// CHECK-EMPTY:

  .text
g:
.Lr0:
  .linkfusedread
  movq __imp_v$tls(%rip), %rax
  .linkfusedaccess .Lr0, %rax
.Lr1:
  .linkfusedread
  movl 8(%rax), %r9d
  .linkfusedindex .Lr1, %r9
  addq %r9, %rbx
  .linkfusedindex .Lr1, %r9
  addq %rcx, %r9
  retq

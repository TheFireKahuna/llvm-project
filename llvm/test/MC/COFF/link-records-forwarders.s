// .linkforwarder says that a function only jumps through the import pointer
// of an equivalent function: a group of the non-critical kind 6 that pairs
// the forwarder's symbol index with the pointer's.

// RUN: llvm-mc -triple x86_64-pc-windows-msvc -filetype=obj %s -o %t.o
// RUN: llvm-objdump -s -j .llvm_link_records %t.o | FileCheck %s
// RUN: llvm-mc -triple x86_64-pc-windows-msvc %s | \
// RUN:   FileCheck --check-prefix=ASM %s

// "LLRC", version 1, no capabilities, then a group of kind 6 and 2 bytes:
// symbol 8, fwd, and symbol 11, __imp_base.
// CHECK:      Contents of section .llvm_link_records:
// CHECK-NEXT: 0000 4c4c5243 01000602 080b

// ASM: .linkforwarder fwd, __imp_base

  .section .text,"xr",discard,fwd
  .globl fwd
fwd:
  .linkforwarder fwd, __imp_base
  rex64 jmpq *__imp_base(%rip)

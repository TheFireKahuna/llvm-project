// .linkifunc says that a function is an indirect function, a stub that jumps
// through its import pointer, which the image's start-up writes with what the
// resolver returns: a group of the critical kind 39 that pairs the function's
// symbol index with the resolver's, both registered even if nothing else
// names them.

// RUN: llvm-mc -triple x86_64-unknown-windows-itanium -filetype=obj %s -o %t.o
// RUN: llvm-objdump -s -j .llvm_link_records %t.o | FileCheck %s
// RUN: llvm-objdump -t %t.o | FileCheck --check-prefix=SYMS %s
// RUN: llvm-mc -triple x86_64-unknown-windows-itanium %s | \
// RUN:   FileCheck --check-prefix=ASM %s

// "LLRC", version 1, the capabilities and the site records, then a group of
// kind 39 and 2 bytes: symbol 8, f, and symbol 10, resolve.
// CHECK:      Contents of section .llvm_link_records:
// CHECK-NEXT: 0000 4c4c5243 01070203 0001221e 03000000
// CHECK-NEXT: 0010 2702080a

// SYMS: resolve

// ASM: .linkifunc f, resolve

  .text
  .globl f
f:
  jmpq *__imp_f(%rip)
  .linkifunc f, resolve

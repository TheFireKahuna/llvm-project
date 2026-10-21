// .linkifuncfallback names the function an indirect function's import pointer
// holds until the image's start-up writes it: a group of the non-critical kind
// 40 that pairs the function's symbol index with the fallback's, both
// registered even if nothing else names them.

// RUN: llvm-mc -triple x86_64-unknown-windows-itanium -filetype=obj %s -o %t.o
// RUN: llvm-objdump -s -j .llvm_link_records %t.o | FileCheck %s
// RUN: llvm-objdump -t %t.o | FileCheck --check-prefix=SYMS %s
// RUN: llvm-mc -triple x86_64-unknown-windows-itanium %s | \
// RUN:   FileCheck --check-prefix=ASM %s

// After kind 39's group, one of kind 40 and 2 bytes: symbol 8, f, and symbol
// 11, fallback.
// CHECK:      Contents of section .llvm_link_records:
// CHECK-NEXT: 0000 4c4c5243 01070203 0001221e 03000000
// CHECK-NEXT: 0010 2702080a 2802080b

// SYMS: fallback

// ASM: .linkifunc f, resolve
// ASM: .linkifuncfallback f, fallback

  .text
  .globl f
f:
  jmpq *__imp_f(%rip)
  .linkifunc f, resolve
  .linkifuncfallback f, fallback

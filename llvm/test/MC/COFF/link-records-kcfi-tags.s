// .linkkcfitag says that the KCFI prefix its symbol marks holds a membership
// tag in the word before its marker, not a second type: a group of the
// non-critical kind 10 that lists the symbols' indices.

// RUN: llvm-mc -triple x86_64-pc-windows-msvc -filetype=obj %s -o %t.o
// RUN: llvm-objdump -s -j .llvm_link_records %t.o | FileCheck %s
// RUN: llvm-mc -triple x86_64-pc-windows-msvc %s | \
// RUN:   FileCheck --check-prefix=ASM %s

// "LLRC", version 1, no capabilities, then a group of kind 10 and 1 byte:
// symbol 8, __cfi_tagged.
// CHECK:      Contents of section .llvm_link_records:
// CHECK-NEXT: 0000 4c4c5243 01000a01 08

// ASM: .linkkcfitag __cfi_tagged

  .text
  .p2align 4
__cfi_tagged:
  .linkkcfitag __cfi_tagged
  .long 0x11223344
  nopl 0x71c5a06(%rax)
  movl $0x12345678, %eax
  .globl tagged
tagged:
  retq

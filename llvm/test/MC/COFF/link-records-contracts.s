// .linkcontract says that the code in a section requires an ABI contract of
// the image that provides a symbol: a group of the critical kind 7 holding the
// distinct contracts, then, for each section, its symbols and the indices of
// the contracts each requires.

// RUN: llvm-mc -triple x86_64-pc-windows-msvc -filetype=obj %s -o %t.o
// RUN: llvm-objdump -s -j .llvm_link_records %t.o | FileCheck %s
// RUN: llvm-mc -triple x86_64-pc-windows-msvc %s | \
// RUN:   FileCheck --check-prefix=ASM %s
// RUN: not llvm-mc -triple x86_64-pc-windows-msvc -filetype=obj \
// RUN:   --defsym ERR=1 %s -o /dev/null 2>&1 | \
// RUN:   FileCheck --check-prefix=ERR %s

// "LLRC", version 1, no capabilities, then a group of kind 7 and 74 bytes:
// three contracts, the type 1T, the type 1U and the datum d, each with its
// 16-byte digest; then the section of f1 (symbol 6), whose code requires 1T
// and 1U of __imp_g (symbol 14) and d of __imp_d (symbol 15), and that of f2
// (symbol 9), whose code requires 1T of __imp_g. A contract named twice is
// listed once, and a row whose symbol nothing references, or whose owner is
// not defined, is dropped.
// CHECK:      Contents of section .llvm_link_records:
// CHECK-NEXT: 0000 4c4c5243 0100074a 03000231 54000102
// CHECK-NEXT: 0010 03040506 0708090a 0b0c0d0e 0f000231
// CHECK-NEXT: 0020 55f0f1f2 f3f4f5f6 f7f8f9fa fbfcfdfe
// CHECK-NEXT: 0030 ff010164 10111213 14151617 18191a1b
// CHECK-NEXT: 0040 1c1d1e1f 06020e02 00010f01 0209010e
// CHECK-NEXT: 0050 0100

// ASM: .linkcontract f1, __imp_g, type, "1T", "000102030405060708090a0b0c0d0e0f"
// ASM: .linkcontract f1, __imp_d, data, "d", "101112131415161718191a1b1c1d1e1f"

  .section .text,"xr",discard,f1
  .globl f1
f1:
  .linkcontract f1, __imp_g, type, "1T", "000102030405060708090a0b0c0d0e0f"
  .linkcontract f1, __imp_g, type, "1U", "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff"
  .linkcontract f1, __imp_d, data, "d", "101112131415161718191a1b1c1d1e1f"
  .linkcontract f1, __imp_unused, type, "1T", "000102030405060708090a0b0c0d0e0f"
  .linkcontract f1, __imp_g, type, "1T", "000102030405060708090a0b0c0d0e0f"
  callq *__imp_g(%rip)
  movq __imp_d(%rip), %rax
  retq

  .section .text,"xr",discard,f2
  .globl f2
f2:
  .linkcontract f2, __imp_g, type, "1T", "000102030405060708090a0b0c0d0e0f"
  .linkcontract undefined_owner, __imp_g, type, "1V", "202122232425262728292a2b2c2d2e2f"
  rex64 jmpq *__imp_g(%rip)

.ifdef ERR
// ERR: [[#@LINE+1]]:30: error: expected 'type' or 'data'
  .linkcontract f2, __imp_g, layout, "1T", "000102030405060708090a0b0c0d0e0f"
// ERR: [[#@LINE+1]]:42: error: digest must be 32 hexadecimal digits
  .linkcontract f2, __imp_g, type, "1T", "0001"
.endif

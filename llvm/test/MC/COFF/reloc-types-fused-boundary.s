// RUN: llvm-mc -triple x86_64-unknown-windows-itanium -filetype=obj \
// RUN:   -x86-align-branch-boundary=32 -x86-align-branch=jcc+fused %s -o %t.o
// RUN: llvm-readobj -r %t.o | FileCheck %s
// RUN: llvm-objdump -d %t.o | FileCheck %s --check-prefix=DIS

// A fused test and its je or jne are one unit to branch-boundary padding,
// which goes before the test (and its marker): a register test as a fused
// branch, a compare in memory as a jcc. The ALIGN index states the unit's
// length: 23069157 is 32 bytes and 5 bytes, and 44040677 is 10 bytes.

// CHECK:      0x1F IMAGE_REL_LLVM_ALIGN - (23069157)
// CHECK-NEXT: 0x20 IMAGE_REL_LLVM_FUSED_TEST_REG __imp_f
// CHECK-NEXT: 0x24 IMAGE_REL_LLVM_JUMP8
// CHECK-NEXT: 0x3A IMAGE_REL_LLVM_ALIGN - (44040677)
// CHECK-NEXT: 0x43 IMAGE_REL_LLVM_FUSED_TEST __imp_f
// CHECK-NEXT: 0x49 IMAGE_REL_LLVM_JUMP8

// DIS:      1f: 90                      nop
// DIS-NEXT: 20: 48 85 c0                testq %rax, %rax
// DIS-NEXT: 23: 74 00                   je
// DIS:      3a: 66 0f 1f 44 00 00       nopw
// DIS-NEXT: 40: 48 83 3d 00 00 00 00 00 cmpq
// DIS-NEXT: 48: 75 00                   jne

  .text
f:
  .rept 24
  nop
  .endr
  movq __imp_f(%rip), %rax
  .reloc ., IMAGE_REL_LLVM_FUSED_TEST_REG, __imp_f
  testq %rax, %rax
  je f
  .rept 21
  nop
  .endr
  cmpq $0, __imp_f@FUSED(%rip)
  jne f
  retq

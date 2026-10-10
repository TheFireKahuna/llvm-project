// An indirect jump's site counts its prefixes where the assembler may still
// pad instructions to align branches: a prefix on a line of its own before the
// jump, and prefixes the assembler adds to the jump itself as padding.

// RUN: llvm-mc -triple x86_64-unknown-windows-itanium -filetype=obj %s \
// RUN:   -x86-align-branch-boundary=32 -x86-align-branch=jcc \
// RUN:   -x86-pad-max-prefix-size=5 -o %t.o
// RUN: llvm-objdump -d %t.o | FileCheck %s --check-prefix=DIS
// RUN: llvm-objdump -s -j .llvm_link_records %t.o | FileCheck %s

// DIS:      0: 48 ff 25 00 00 00 00 jmpq
// DIS:     39: 2e ff 25 00 00 00 00 jmpq
// DIS:     76: 2e 2e 2e 2e ff 25 00 00 00 00 jmpq

// One group of 3 sites: 0x3 a jump after one prefix (form 5), 0x3c likewise,
// and 0x7c other (form 0), after four.
// CHECK:      Contents of section .llvm_link_records:
// CHECK-NEXT: 0000 4c4c5243 01030207 00033595 078008
// CHECK-EMPTY:

  .text
top:
  rex64
  jmpq *__imp_g(%rip)
  .p2align 5
  movl $1, %eax
  movl $1, %eax
  movl $1, %eax
  movl $1, %eax
  movl $1, %eax
  jmpq *__imp_g(%rip)
  jne top
  .p2align 5
  movl $1, %eax
  movl $1, %eax
  movl $1, %eax
  movl $1, %eax
  testl %eax, %eax
  jmpq *__imp_g(%rip)
  jne far

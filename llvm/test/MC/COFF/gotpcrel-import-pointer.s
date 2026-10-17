// On Windows Itanium and NT-POSIX a symbol's import pointer, __imp_X, is its
// GOT entry, so X@GOTPCREL, which position-independent ELF assembly uses to
// reach external data and functions, reaches __imp_X, and the object describes
// the instruction as it describes the compiler's own loads of an import
// pointer.

// RUN: llvm-mc -triple x86_64-unknown-windows-itanium -filetype=obj %s -o %t.o
// RUN: llvm-objdump -dr %t.o | FileCheck %s
// RUN: llvm-objdump -s -j .llvm_link_records %t.o | FileCheck %s --check-prefix=REC
// RUN: llvm-mc -triple x86_64-pc-windows-ntposix -filetype=obj %s -o %t.n.o
// RUN: llvm-objdump -dr %t.n.o | FileCheck %s
// RUN: llvm-objdump -s -j .llvm_link_records %t.n.o | FileCheck %s --check-prefix=REC

// CHECK:      movq (%rip), %rax
// CHECK-NEXT:   IMAGE_REL_AMD64_REL32 __imp_v
// CHECK-NEXT: callq *(%rip)
// CHECK-NEXT:   IMAGE_REL_AMD64_REL32 __imp_g
// CHECK-NEXT: movq 0x8(%rip), %rcx
// CHECK-NEXT:   IMAGE_REL_AMD64_REL32 __imp_v

// "LLRC", version 1, the capabilities, then one group of kind 2: .text's symbol
// with three sites, a load at 3, a call at 9 and a load at 0x10.
// REC:      Contents of section .llvm_link_records:
// REC-NEXT: 0000 4c4c5243 01070205 00033361 70

  .text
  movq v@GOTPCREL(%rip), %rax
  callq *g@GOTPCREL(%rip)
  movq v@GOTPCREL+8(%rip), %rcx
  retq

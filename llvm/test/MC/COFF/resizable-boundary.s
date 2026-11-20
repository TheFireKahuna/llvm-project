// RUN: llvm-mc -triple x86_64-unknown-windows-itanium -filetype=obj \
// RUN:   -x86-align-branch-boundary=32 -x86-align-branch=jcc+fused+jmp \
// RUN:   %s -o %t.o
// RUN: llvm-readobj -r --coff-link-records %t.o | FileCheck %s
// RUN: llvm-objdump -d -j .text %t.o | FileCheck %s --check-prefix=DIS
// RUN: llvm-mc -triple x86_64-unknown-windows-itanium -filetype=obj \
// RUN:   -x86-align-branch-boundary=32 -x86-align-branch=jcc+fused+jmp \
// RUN:   -x86-pad-max-prefix-size=5 %s -o %t.p.o
// RUN: cmp %t.o %t.p.o
// RUN: llvm-mc -triple x86_64-pc-windows-msvc -filetype=obj \
// RUN:   -x86-align-branch-boundary=32 -x86-align-branch=jcc+fused+jmp \
// RUN:   -x86-pad-max-prefix-size=5 %s -o %t.m.o
// RUN: llvm-readobj -r %t.m.o | FileCheck %s --check-prefix=MSVC
// RUN: llvm-objdump -d -j .text %t.m.o | FileCheck %s --check-prefix=MSVC-DIS

// Padding that keeps a branch from crossing or ending on a boundary is the
// linker's to redo in a section it resizes. An ALIGN relocation at the padding,
// also where there is none, adds bit 21 and, from bit 22, the length of the
// branch, with the instruction fused to it, to the boundary and the longest
// no-operation: 23069157 is 32 bytes, 15 and 5 bytes, and 10486245 is 2 bytes.
// The padding is no-operations, also where other code would lengthen the
// instructions before it with prefixes.

// CHECK:      Relocations [
// CHECK-NEXT:   Section ({{[0-9]+}}) .text {
// CHECK-NEXT:     0x1B IMAGE_REL_LLVM_ALIGN - (23069157)
// CHECK-NEXT:     0x24 IMAGE_REL_LLVM_JUMP8 [[T:\$L[0-9a-f]+]]
// CHECK-NEXT:     0x3D IMAGE_REL_LLVM_ALIGN - (10486245)
// CHECK-NEXT:     0x3E IMAGE_REL_LLVM_JUMP8 [[T]]
// CHECK-NEXT:     0x57 IMAGE_REL_LLVM_ALIGN - (23069157)
// CHECK-NEXT:     0x58 IMAGE_REL_LLVM_BRANCH_TAIL ext
// CHECK-NEXT:     0x5C IMAGE_REL_LLVM_ALIGN - (10486245)
// CHECK-NEXT:     0x5D IMAGE_REL_LLVM_JUMP8 [[T]]
// CHECK-NEXT:     0x60 IMAGE_REL_LLVM_MEM_CALL __imp_g
// CHECK-NEXT:   }

// What follows the padding in .text$u is longer than 5 bits hold, and in
// .text$v its length depends on the layout, so neither padding has a
// relocation and neither section is listed. The assembler resolves the
// jumps in them and their alignment.
// CHECK-NEXT:   Section ({{[0-9]+}}) .text$u {
// CHECK-NEXT:     0x2 IMAGE_REL_LLVM_MEM_CALL __imp_g
// CHECK-NEXT:   }
// CHECK-NEXT:   Section ({{[0-9]+}}) .text$v {
// CHECK-NEXT:     0x2 IMAGE_REL_LLVM_MEM_CALL __imp_g
// CHECK-NEXT:   }
// CHECK:          Kind: Sections (0x2)
// CHECK-NEXT:     ResizableSection: .text ({{[0-9]+}})
// CHECK-NEXT:   }

// DIS:      18: 48 89 c1       movq %rax, %rcx
// DIS-NEXT: 1b: 0f 1f 44 00 00 nopl (%rax,%rax)
// DIS-NEXT: 20: 48 39 c1       cmpq %rax, %rcx
// DIS-NEXT: 23: 74 00          je

// Another triple's object has no such relocation, and prefixes for padding.
// MSVC-NOT: IMAGE_REL_LLVM
// MSVC:     0x58 IMAGE_REL_AMD64_REL32 ext
// MSVC-DIS:      19: 2e 2e 2e 2e 48 89 c1 movq %rax, %rcx
// MSVC-DIS-NEXT: 20: 48 39 c1             cmpq %rax, %rcx

  .text
  .globl f
f:
  .rept 9
  movq %rax, %rcx
  .endr
  cmpq %rax, %rcx
  je .Lt
  .rept 8
  movq %rax, %rcx
  .endr
  jmp .Lt
  .rept 8
  movq %rax, %rcx
  .endr
  jmp ext
.Lt:
  jne .Lt
  callq *__imp_g(%rip)
  retq

  .section .text$u,"xr"
  callq *__imp_g(%rip)
  cmpq %rax, %rcx
  .fill 30, 1, 0x90
  je .Lu
.Lu:
  retq

  .section .text$v,"xr"
  callq *__imp_g(%rip)
  cmpq %rax, %rcx
  .p2align 4
  je .Lv
.Lv:
  retq

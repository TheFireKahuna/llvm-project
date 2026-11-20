// RUN: llvm-mc -triple x86_64-unknown-windows-itanium -mattr=+egpr \
// RUN:   -filetype=obj %s -o %t.o
// RUN: llvm-readobj -r %t.o | FileCheck %s
// RUN: llvm-mc -triple x86_64-pc-windows-ntposix -mattr=+egpr -filetype=obj \
// RUN:   %s -o %t.n.o
// RUN: llvm-readobj -r %t.n.o | FileCheck %s
// RUN: llvm-mc -triple x86_64-pc-windows-msvc -mattr=+egpr -filetype=obj %s \
// RUN:   -o %t.m.o
// RUN: llvm-readobj -r %t.m.o | \
// RUN:   FileCheck %s --check-prefix=MSVC --implicit-check-not=IMAGE_REL_LLVM

// On Windows Itanium and NT-POSIX the relocation type of a field in an
// instruction states the instruction's form: a direct branch and whether the
// assembler chose its width, a call, jump or load through memory, or an lea.
// The types of branches are pinned in reloc-types-tail-jump.s.
// The form is stated for a reference to a symbol through RIP with no segment
// register, and for a call, jump or load only where it has no offset. A
// call's prefixes are not part of its form, and an lea has any operand size.
// Any other field in code is REL32, as every one is elsewhere.

// MSVC: IMAGE_REL_AMD64_REL32 __imp_g

// CHECK:      Section ({{[0-9]+}}) .text {
// CHECK-NEXT:   0x2 IMAGE_REL_LLVM_MEM_CALL __imp_g
// CHECK-NEXT:   0x8 IMAGE_REL_LLVM_MEM_JUMP __imp_g
// CHECK-NEXT:   0xF IMAGE_REL_LLVM_MEM_JUMP_P1 __imp_g
// CHECK-NEXT:   0x16 IMAGE_REL_LLVM_MEM_LOAD __imp_g
// CHECK-NEXT:   0x1D IMAGE_REL_LLVM_MEM_LOAD __imp_g
// CHECK-NEXT:   0x25 IMAGE_REL_LLVM_MEM_LOAD_REX2 __imp_g
// CHECK-NEXT:   0x2C IMAGE_REL_LLVM_LEA g
// CHECK-NEXT:   0x34 IMAGE_REL_LLVM_LEA g
// CHECK-NEXT:   0x3B IMAGE_REL_LLVM_LEA_OFFSET g
// CHECK-NEXT:   0x41 IMAGE_REL_LLVM_LEA g
// CHECK-NEXT:   0x48 IMAGE_REL_LLVM_LEA g
// CHECK-NEXT:   0x4F IMAGE_REL_LLVM_MEM_CALL __imp_g
// CHECK-NEXT:   0x56 IMAGE_REL_LLVM_MEM_CALL __imp_g
// CHECK-NEXT:   0x5D IMAGE_REL_AMD64_REL32 __imp_g
// CHECK-NEXT:   0x64 IMAGE_REL_AMD64_REL32_1 __imp_g
// CHECK-NEXT:   0x6C IMAGE_REL_AMD64_REL32 __imp_g
// CHECK-NEXT:   0x74 IMAGE_REL_AMD64_REL32 __imp_g
// CHECK-NEXT:   0x7C IMAGE_REL_AMD64_REL32 __imp_g
// CHECK-NEXT:   0x83 IMAGE_REL_AMD64_REL32 __imp_g
// CHECK-NEXT:   0x8A IMAGE_REL_AMD64_REL32 __imp_g
// CHECK-NEXT:   0x92 IMAGE_REL_AMD64_REL32 __imp_g
// CHECK-NEXT:   0x9A IMAGE_REL_AMD64_REL32 g
// CHECK-NEXT:   0x9F IMAGE_REL_LLVM_BRANCH32 g
// CHECK-NEXT:   0xA3 IMAGE_REL_AMD64_REL32 g
// CHECK-NEXT:   0xA7 IMAGE_REL_AMD64_ADDR64 h
// CHECK-NEXT: }

  .text
  .globl f
f:
  callq *__imp_g(%rip)
  jmpq *__imp_g(%rip)
  rex64 jmpq *__imp_g(%rip)
  movq __imp_g(%rip), %rax
  movq __imp_g(%rip), %r9
  movq __imp_g(%rip), %r16
  leaq g(%rip), %rcx
  leaq g(%rip), %r17
  leaq g+16(%rip), %rcx
  leal g(%rip), %eax
  leaw g(%rip), %ax
  notrack callq *__imp_g(%rip)
  rex64 callq *__imp_g(%rip)

// Another instruction, one with an immediate, and a load with an offset.
  cmpq %rax, __imp_g(%rip)
  cmpq $1, __imp_g(%rip)
  movq __imp_g+8(%rip), %rax

// A second prefix before a jump, a segment register and a 32-bit base.
  notrack rex64 jmpq *__imp_g(%rip)
  movq %fs:__imp_g(%rip), %rax
  callq *%gs:__imp_g(%rip)
  jmpq *%fs:__imp_g(%rip)
  movq __imp_g(%eip), %rax
  leaq g(%eip), %rax

// A call has its type with @PLT too, and a data field is REL32.
  callq g@PLT
  .long g - .
  .quad h

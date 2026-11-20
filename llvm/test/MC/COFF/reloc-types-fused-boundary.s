// RUN: llvm-mc -triple x86_64-unknown-windows-itanium -filetype=obj \
// RUN:   -x86-align-branch-boundary=32 \
// RUN:   -x86-align-branch=jcc+fused+jmp+call+indirect+ret %s -o %t.o
// RUN: llvm-readobj -r --coff-link-records %t.o | \
// RUN:   FileCheck %s --check-prefixes=CHECK,NOPREL
// RUN: llvm-objdump -d %t.o | FileCheck %s --check-prefixes=DIS,NOP
// RUN: llvm-mc -triple x86_64-unknown-windows-itanium -filetype=obj \
// RUN:   -x86-align-branch-boundary=32 \
// RUN:   -x86-align-branch=jcc+fused+jmp+call+indirect+ret \
// RUN:   -x86-pad-max-prefix-size=5 %s -o %t.p.o
// RUN: llvm-readobj -r --coff-link-records %t.p.o | \
// RUN:   FileCheck %s --check-prefixes=CHECK,PREFIX
// RUN: llvm-objdump -d %t.p.o | FileCheck %s --check-prefixes=DIS,PFX

// A fused call or jump is one unit to the padding that keeps a branch from
// crossing or ending on a boundary, as the linker rewrites it in place from
// its marker. The padding is before the marker, and none of it is a prefix
// on an instruction of the site. In a section the linker resizes its ALIGN
// relocation states the length of the site: 27263461 is 32 bytes, 15 and 6
// bytes, and 31457765 is 7 bytes. The field of a fused read before such
// padding is zero.

// CHECK:      Relocations [
// CHECK-NEXT:   Section ({{[0-9]+}}) .text {
// CHECK-NEXT:     0x17 IMAGE_REL_LLVM_FUSED_READ __imp_f
// CHECK-NEXT:     0x1B IMAGE_REL_LLVM_ALIGN - (27263461)
// CHECK-NEXT:     0x20 IMAGE_REL_LLVM_FUSED_CALL __imp_f
// CHECK-NEXT:     0x26 IMAGE_REL_LLVM_ALIGN - (10486245)
// CHECK-NEXT:     0x27 IMAGE_REL_LLVM_JUMP8
// CHECK-NEXT:     0x35 IMAGE_REL_LLVM_FUSED_READ __imp_f
// CHECK-NEXT:     0x39 IMAGE_REL_LLVM_ALIGN - (31457765)
// CHECK-NEXT:     0x40 IMAGE_REL_LLVM_FUSED_JUMP __imp_f
// CHECK-NEXT:     0x4A IMAGE_REL_LLVM_FUSED_READ __imp_f
// CHECK-NEXT:     0x4E IMAGE_REL_LLVM_ALIGN - (27263461)
// CHECK-NEXT:     0x4E IMAGE_REL_LLVM_FUSED_CALL __imp_f
// CHECK-NEXT:     0x54 IMAGE_REL_LLVM_ALIGN - (6291941)
// CHECK-NEXT:   }

// A section the linker does not resize has no such relocation, and its
// padding may be prefixes on the instructions before the fused read.
// CHECK-NEXT:   Section ({{[0-9]+}}) .text$n {
// NOPREL-NEXT:    0x17 IMAGE_REL_LLVM_FUSED_READ __imp_f
// PREFIX-NEXT:    0x1C IMAGE_REL_LLVM_FUSED_READ __imp_f
// CHECK-NEXT:     0x20 IMAGE_REL_LLVM_FUSED_CALL __imp_f
// CHECK-NEXT:     0x54 IMAGE_REL_LLVM_FUSED_READ __imp_f
// CHECK-NEXT:     0x58 IMAGE_REL_LLVM_FUSED_JUMP __imp_f
// CHECK-NEXT:   }
// CHECK:          ResizableSection: .text ({{[0-9]+}})
// CHECK-NEXT:   }
// CHECK-NEXT:   Flags: 0x8

// DIS:      14: 48 8b 05 00 00 00 00 movq
// DIS-NEXT: 1b: 0f 1f 44 00 00       nopl
// DIS-NEXT: 20: 2e 2e 2e 2e ff d0    callq *%rax
// DIS-NEXT: 26: 75 00                jne
// DIS:      32: 48 8b 05 00 00 00 00 movq
// DIS-NEXT: 39: 0f 1f 80 00 00 00 00 nopl
// DIS-NEXT: 40: 48 ff e0             jmpq *%rax
// DIS-COUNT-4:  cc                   int3
// DIS-NEXT: 47: 4c 8b 05 00 00 00 00 movq
// DIS-NEXT: 4e: 2e 2e 2e 41 ff d0    callq *%r8
// DIS-NEXT: 54: c3                   retq

// DIS-LABEL: section .text$n:
// NOP:      11: 48 89 c1             movq %rax, %rcx
// NOP-NEXT: 14: 48 8b 05 00 00 00 00 movq
// NOP-NEXT: 1b: 0f 1f 44 00 00       nopl
// PFX:       e: 2e 48 89 c1          movq %rax, %rcx
// PFX-NEXT: 12: 2e 2e 2e 2e 48 89 c1 movq %rax, %rcx
// PFX-NEXT: 19: 48 8b 05 00 00 00 00 movq
// DIS-NEXT: 20: 2e 2e 2e 2e ff d0    callq *%rax
// DIS-NEXT: 26: c3                   retq
// DIS:      51: 48 8b 05 00 00 00 00 movq
// DIS-NEXT: 58: 48 ff e0             jmpq *%rax
// DIS-COUNT-4:  cc                   int3
// DIS-NEXT: 5f: 90                   nop
// DIS-NEXT: 60: ff d1                callq *%rcx

  .text
f:
  movq $1, %rcx
  movq $1, %rcx
  movq %rax, %rcx
  movq %rax, %rcx
  movq __imp_f@FUSED(%rip), %rax
.Lcall:
  .reloc .Lcall, IMAGE_REL_LLVM_FUSED_CALL, __imp_f
  cs; cs; cs; cs
  callq *%rax
  jne f
  movq $1, %rcx
  movq %rax, %rcx
  movq __imp_f@FUSED(%rip), %rax
  .reloc ., IMAGE_REL_LLVM_FUSED_JUMP, __imp_f
  rex64 jmpq *%rax
  int3; int3; int3; int3
  movq __imp_f@FUSED(%rip), %r8
  .reloc ., IMAGE_REL_LLVM_FUSED_CALL, __imp_f
  cs; cs; cs
  callq *%r8
  retq

  .section .text$n,"xr"
  movq $1, %rcx
  movq $1, %rcx
  movq %rax, %rcx
  movq %rax, %rcx
  movq __imp_f@FUSED(%rip), %rax
  .reloc ., IMAGE_REL_LLVM_FUSED_CALL, __imp_f
  cs; cs; cs; cs
  callq *%rax
  retq
  .p2align 5
  movq $1, %rcx
  movq $1, %rcx
  movq %rax, %rcx
  movq __imp_f@FUSED(%rip), %rax
  .reloc ., IMAGE_REL_LLVM_FUSED_JUMP, __imp_f
  rex64 jmpq *%rax
  int3; int3; int3; int3
  callq *%rcx
  retq

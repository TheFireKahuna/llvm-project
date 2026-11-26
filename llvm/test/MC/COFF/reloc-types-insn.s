// RUN: llvm-mc -triple x86_64-unknown-windows-itanium -filetype=obj %s -o %t.o
// RUN: llvm-readobj -r --expand-relocs %t.o | FileCheck %s
// RUN: obj2yaml %t.o | yaml2obj | llvm-readobj -r --expand-relocs - \
// RUN:   | FileCheck %s
// RUN: llvm-mc -triple x86_64-pc-windows-msvc -filetype=obj %s -o %t.m.o
// RUN: llvm-readobj -r %t.m.o | FileCheck %s --check-prefix=MSVC \
// RUN:   --implicit-check-not=IMAGE_REL_LLVM

// On Windows Itanium and NT-POSIX a field relative to rip that no other type
// states is REL32_INSN, 0x8100 plus the bytes of its instruction before the
// field times 16 plus those of its immediate after it, a store's too. The
// bytes before count a prefix written as an instruction of its own directly
// before it; where a label comes between, or data whose bytes may be
// prefixes, the field is REL32 and states nothing.

// MSVC-COUNT-12: IMAGE_REL_AMD64_REL32

// 0x8120, 0x8130, 0x8121, 0x8150.
// CHECK:      Offset: 0x2
// CHECK-NEXT: Type: IMAGE_REL_LLVM_REL32_INSN (33056)
// CHECK:      Offset: 0x9
// CHECK-NEXT: Type: IMAGE_REL_LLVM_REL32_INSN (33072)
// CHECK:      Offset: 0xF
// CHECK-NEXT: Type: IMAGE_REL_LLVM_REL32_INSN (33057)
// CHECK:      Offset: 0x19
// CHECK-NEXT: Type: IMAGE_REL_LLVM_REL32_INSN (33104)
  movl t+8(%rip), %eax
  cmpq %rcx, t(%rip)
  cmpb $0, t(%rip)
  vpbroadcastq t(%rip), %ymm1

// A store: 0x8130.
// CHECK:      Offset: 0x20
// CHECK-NEXT: Type: IMAGE_REL_LLVM_REL32_INSN (33072)
  movq %rax, t(%rip)

// lock on the line before, after a semicolon and as the instruction's own
// prefix: each 0x8150, starting at the lock.
// CHECK:      Offset: 0x29
// CHECK-NEXT: Type: IMAGE_REL_LLVM_REL32_INSN (33104)
// CHECK:      Offset: 0x32
// CHECK-NEXT: Type: IMAGE_REL_LLVM_REL32_INSN (33104)
// CHECK:      Offset: 0x3B
// CHECK-NEXT: Type: IMAGE_REL_LLVM_REL32_INSN (33104)
  lock
  cmpxchgq %rcx, t(%rip)
  lock; cmpxchgq %rcx, t(%rip)
  lock cmpxchgq %rcx, t(%rip)

// Two prefixes written alone: 0x8140.
// CHECK:      Offset: 0x43
// CHECK-NEXT: Type: IMAGE_REL_LLVM_REL32_INSN (33088)
  rep; data16; movl t(%rip), %eax

// A REX.W load after an immediate of 0x48: 0x8130, from the REX.
// CHECK:      Offset: 0x4C
// CHECK-NEXT: Type: IMAGE_REL_LLVM_REL32_INSN (33072)
  movb $0x48, %al
  movq t+8(%rip), %rax

// A label between a prefix and its instruction, and data before one.
// CHECK:      Offset: 0x55
// CHECK-NEXT: Type: IMAGE_REL_AMD64_REL32 (4)
// CHECK:      Offset: 0x5D
// CHECK-NEXT: Type: IMAGE_REL_AMD64_REL32 (4)
  lock
.Lx:
  cmpxchgq %rcx, t(%rip)
  .byte 0x2e
  cmpq %rcx, t(%rip)

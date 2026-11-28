// RUN: llvm-mc -triple x86_64-unknown-windows-itanium -mattr=+egpr \
// RUN:   -filetype=obj %s -o %t.o
// RUN: llvm-readobj -r --coff-link-records %t.o | FileCheck %s
// RUN: llvm-objdump -d %t.o | FileCheck %s --check-prefix=DIS
// RUN: llvm-mc -triple x86_64-pc-windows-ntposix -mattr=+egpr %s | \
// RUN:   llvm-mc -triple x86_64-pc-windows-ntposix -mattr=+egpr \
// RUN:   -filetype=obj -o %t.s.o
// RUN: llvm-readobj -r --coff-link-records %t.s.o | FileCheck %s
// RUN: llvm-objdump -d %t.s.o | FileCheck %s --check-prefix=DIS
// RUN: not llvm-mc -triple x86_64-unknown-windows-itanium -filetype=obj \
// RUN:   -defsym=READ=1 %s -o /dev/null 2>&1 | \
// RUN:   FileCheck %s --check-prefix=READ --implicit-check-not=error:
// RUN: not llvm-mc -triple x86_64-pc-windows-msvc -filetype=obj %s \
// RUN:   -o /dev/null 2>&1 | FileCheck %s --check-prefix=MSVC

// On Windows Itanium and NT-POSIX @FUSED makes the load of a pointer a fused
// read and its compare with zero, before a je or jne, a fused test. The
// assembler says in the flags of the object's link records that it checked
// them. The linker may delete a read, so the section that holds one is
// listed as resizable and no distance across one is the assembler's: a jump
// over one has a relocation, and so has a lea of a label beyond one.

// CHECK:      Section ({{[0-9]+}}) .text {
// CHECK-NEXT:   0x3 IMAGE_REL_LLVM_FUSED_READ __imp_f
// CHECK-NEXT:   0xD IMAGE_REL_LLVM_FUSED_READ_REX2 __imp_f
// CHECK-NEXT:   0x15 IMAGE_REL_LLVM_FUSED_READ __imp_f
// CHECK-NEXT:   0x20 IMAGE_REL_LLVM_FUSED_READ __imp_f
// CHECK-NEXT:   0x2A IMAGE_REL_LLVM_FUSED_READ __imp_f
// CHECK-NEXT:   0x2F IMAGE_REL_LLVM_JUMP8
// CHECK-NEXT:   0x33 IMAGE_REL_LLVM_FUSED_READ __imp_f
// CHECK-NEXT:   0x43 IMAGE_REL_LLVM_FUSED_READ __imp_f
// CHECK-NEXT:   0x4D IMAGE_REL_LLVM_FUSED_TEST __imp_f
// CHECK-NEXT:   0x53 IMAGE_REL_LLVM_JUMP8
// CHECK-NEXT: }
// CHECK-NEXT: Section ({{[0-9]+}}) .text$r {
// CHECK-NEXT:   0x3 IMAGE_REL_LLVM_FUSED_READ __imp_f
// CHECK-NEXT:   0xA IMAGE_REL_AMD64_SREL32
// CHECK-NEXT: }
// CHECK:      ResizableSection: .text (
// CHECK-NEXT: ResizableSection: .text$r (
// CHECK:      Flags: 0x8

// The field of a fused read holds twice the length of the instruction after
// it, with its prefixes, plus one where that transfers control or may fuse
// with a branch that does, or zero where no instruction is seen to follow,
// under the bit that says that a test after the read leaves its flags dead.
// DIS:      48 8b 35 04 00 00 00   movq
// DIS-NEXT: 31 c0                  xorl %eax, %eax
// DIS-NEXT: d5 48 8b 05 03 00 00 00 movq
// DIS-NEXT: c3                     retq
// DIS-NEXT: 48 8b 05 27 00 00 00   movq
// DIS-NEXT: 48 85 c0               testq %rax, %rax
// DIS-NEXT: c3                     retq
// DIS-NEXT: 48 8b 05 00 00 00 00   movq
// DIS:      48 8b 05 00 00 00 00   movq
// DIS:      48 8b 05 00 00 00 00   movq
// DIS-NEXT: 65 48 8b 0c 25 58 00 00 00 movq %gs:0x58, %rcx
// DIS-NEXT: 48 8b 05 00 00 00 00   movq
// DIS-NEXT: 64 8b 09               movl %fs:(%rcx), %ecx
// DIS-NEXT: 48 83 3d 00 00 00 00 00 cmpq

// MSVC: [[#@LINE+5]]:{{[0-9]+}}: error: relocation specifier @FUSED unsupported on COFF targets

  .text
called:
// The instruction after a read writes another register.
  movq __imp_f@FUSED(%rip), %rsi
  xorl %eax, %eax

// A read into one of the registers r16 to r31 has a REX2 prefix.
  movq __imp_f@FUSED(%rip), %r16
  retq

  movq __imp_f@FUSED(%rip), %rax
  testq %rax, %rax
  retq

// Data and a jump whose length the assembler has yet to choose are not seen
// to follow a fused read.
  movq __imp_f@FUSED(%rip), %rax
  .byte 0x90
  xorl %ecx, %ecx
  movq __imp_f@FUSED(%rip), %rax
  jmp called

// Nor is an instruction with a segment override other than CS, as its own
// prefix or a separate one.
  movq __imp_f@FUSED(%rip), %rax
  movq %gs:0x58, %rcx
  movq __imp_f@FUSED(%rip), %rax
  fs
  movl (%rcx), %ecx

// @FUSED makes the 64-bit compare of a pointer with zero a fused test, whose
// field holds its addend less the byte of immediate after it.
  cmpq $0, __imp_f@FUSED(%rip)
  jne called

// A lea of a label has no relocation unless a fused read lies between the
// two.
  .section .text$r,"xr"
.Lread:
  movq __imp_f@FUSED(%rip), %rax
  leaq .Lread(%rip), %rcx
.Lafter:
  leaq .Lafter(%rip), %rcx
  .text

.ifdef READ
// A fused read is a 64-bit load of the pointer itself through RIP, and a
// fused test its 64-bit compare with zero, which has one encoding.
// READ-COUNT-11: error: @FUSED is on neither a 64-bit mov of a pointer nor its 64-bit compare with zero
  movl __imp_f@FUSED(%rip), %eax
  leaq __imp_f@FUSED(%rip), %rax
  movq %rax, __imp_f@FUSED(%rip)
  movq __imp_f@FUSED+8(%rip), %rax
  movq __imp_f@FUSED(%rcx), %rax
  cmpq $1, __imp_f@FUSED(%rip)
  cmpl $0, __imp_f@FUSED(%rip)
  cmpq %rax, __imp_f@FUSED(%rip)
  cmpq $0, __imp_f@FUSED+8(%rip)
  cmpq $0, %fs:__imp_f@FUSED(%rip)
  testq $0, __imp_f@FUSED(%rip)
.endif

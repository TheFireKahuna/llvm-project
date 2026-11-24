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
// RUN:   -defsym=ERR=1 %s -o /dev/null 2>&1 | \
// RUN:   FileCheck %s --check-prefix=ERR --implicit-check-not=error:
// RUN: not llvm-mc -triple x86_64-unknown-windows-itanium -filetype=obj \
// RUN:   -defsym=READ=1 %s -o /dev/null 2>&1 | \
// RUN:   FileCheck %s --check-prefix=READ --implicit-check-not=error:
// RUN: not llvm-mc -triple x86_64-unknown-windows-itanium -filetype=obj \
// RUN:   -defsym=END=1 %s -o /dev/null 2>&1 | \
// RUN:   FileCheck %s --check-prefix=END --implicit-check-not=error:
// RUN: not llvm-mc -triple x86_64-unknown-windows-itanium -filetype=obj \
// RUN:   -x86-align-branch-boundary=32 -x86-align-branch=call+indirect \
// RUN:   -x86-pad-max-prefix-size=5 -defsym=ERR=1 %s -o /dev/null 2>&1 | \
// RUN:   FileCheck %s --check-prefix=ERR --implicit-check-not=error:
// RUN: not llvm-mc -triple x86_64-unknown-windows-itanium -filetype=obj \
// RUN:   -x86-align-branch-boundary=32 -x86-align-branch=call+indirect \
// RUN:   -defsym=END=1 %s -o /dev/null 2>&1 | \
// RUN:   FileCheck %s --check-prefix=END --implicit-check-not=error:
// RUN: not llvm-mc -triple x86_64-pc-windows-msvc -filetype=obj %s \
// RUN:   -o /dev/null 2>&1 | FileCheck %s --check-prefix=MSVC

// On Windows Itanium and NT-POSIX @FUSED makes the load of a pointer a fused
// read. A .reloc of IMAGE_REL_LLVM_FUSED_CALL marks a call through the
// register, behind the CS overrides that make it six bytes, as a fused call,
// and one of IMAGE_REL_LLVM_FUSED_JUMP a jmp with REX.W through it, before
// the int3 that make it seven, as a fused jump. The assembler checks each
// against the instructions and says so in the flags of the object's link
// records.

// CHECK:      Section ({{[0-9]+}}) .text {
// CHECK-NEXT:   0x3 IMAGE_REL_LLVM_FUSED_READ __imp_f
// CHECK-NEXT:   0x9 IMAGE_REL_LLVM_FUSED_CALL __imp_f
// CHECK-NEXT:   0x13 IMAGE_REL_LLVM_FUSED_READ __imp_g
// CHECK-NEXT:   0x17 IMAGE_REL_LLVM_FUSED_CALL __imp_g
// CHECK-NEXT:   0x1D IMAGE_REL_LLVM_FUSED_CALL __imp_g
// CHECK-NEXT:   0x25 IMAGE_REL_LLVM_FUSED_CALL __imp_g
// CHECK-NEXT:   0x30 IMAGE_REL_LLVM_FUSED_READ_REX2 __imp_f
// CHECK-NEXT:   0x34 IMAGE_REL_LLVM_FUSED_CALL __imp_f
// CHECK-NEXT:   0x3E IMAGE_REL_LLVM_FUSED_READ __imp_f
// CHECK-NEXT:   0x45 IMAGE_REL_LLVM_FUSED_JUMP __imp_f
// CHECK-NEXT:   0x4F IMAGE_REL_LLVM_FUSED_READ __imp_f
// CHECK-NEXT:   0x59 IMAGE_REL_LLVM_FUSED_READ __imp_f
// CHECK-NEXT:   0x5E IMAGE_REL_LLVM_JUMP8
// CHECK-NEXT:   0x62 IMAGE_REL_LLVM_FUSED_READ __imp_f
// CHECK-NEXT:   0x72 IMAGE_REL_LLVM_FUSED_READ __imp_f
// CHECK-NEXT:   0x79 IMAGE_REL_LLVM_FUSED_TEST __imp_f
// CHECK-NEXT: }
// CHECK:      Flags: 0x8

// The field of a fused read holds twice the length of the instruction after
// it, with its prefixes, plus one where that transfers control or may fuse
// with a branch that does, or zero where no instruction is seen to follow.
// DIS:      48 8b 35 04 00 00 00   movq
// DIS-NEXT: 31 c0                  xorl %eax, %eax
// DIS-NEXT: 2e 2e 2e 2e ff d6      callq *%rsi
// DIS:      4c 8b 05 0d 00 00 00   movq
// DIS-NEXT: 2e 2e 2e 41 ff d0      callq *%r8
// DIS:      d5 48 8b 05 0d 00 00 00 movq
// DIS-NEXT: 2e 2e d5 10 ff d0      callq *%r16
// DIS:      48 8b 05 07 00 00 00   movq
// DIS-NEXT: 48 85 c0               testq %rax, %rax
// DIS-NEXT: 48 ff e0               jmpq *%rax
// DIS-NEXT: cc                     int3
// DIS-NEXT: cc                     int3
// DIS-NEXT: cc                     int3
// DIS-NEXT: cc                     int3
// DIS-NEXT: 48 8b 05 00 00 00 00   movq
// DIS:      48 8b 05 00 00 00 00   movq
// DIS:      48 8b 05 00 00 00 00   movq
// DIS-NEXT: 65 48 8b 0c 25 58 00 00 00 movq %gs:0x58, %rcx
// DIS-NEXT: 48 8b 05 00 00 00 00   movq
// DIS-NEXT: 64 8b 09               movl %fs:(%rcx), %ecx

// MSVC-DAG: [[#@LINE+6]]:{{[0-9]+}}: error: relocation specifier @FUSED unsupported on COFF targets
// MSVC-DAG: [[#@LINE+7]]:{{[0-9]+}}: error: unknown relocation name

  .text
called:
// A write of another register lies between the load and its call.
  movq __imp_f@FUSED(%rip), %rsi
  xorl %eax, %eax
  .reloc ., IMAGE_REL_LLVM_FUSED_CALL, __imp_f
  cs; cs; cs; cs
  callq *%rsi
  retq

// A call through one of the registers r8 to r15 has a prefix of its own. A
// label and a branch leave the load the last writer. What wrote a register
// before the last call is not the assembler's to see.
  movq __imp_g@FUSED(%rip), %r8
.Lsite:
  .reloc .Lsite, IMAGE_REL_LLVM_FUSED_CALL, __imp_g
  cs; cs; cs
  callq *%r8
  .reloc ., IMAGE_REL_LLVM_FUSED_CALL, __imp_g
  cs; cs; cs
  callq *%r8
  je called
  .reloc ., IMAGE_REL_LLVM_FUSED_CALL, __imp_g
  cs; cs; cs
  callq *%r8
  retq

// So has one through r16 to r31, of two bytes.
  movq __imp_f@FUSED(%rip), %r16
  .reloc ., IMAGE_REL_LLVM_FUSED_CALL, __imp_f
  cs; cs
  callq *%r16
  retq

  movq __imp_f@FUSED(%rip), %rax
  testq %rax, %rax
  .reloc ., IMAGE_REL_LLVM_FUSED_JUMP, __imp_f
  rex64 jmpq *%rax
  int3; int3; int3; int3

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

// A .reloc of IMAGE_REL_LLVM_FUSED_TEST marks the test of a register a load
// of the pointer set.
  .reloc ., IMAGE_REL_LLVM_FUSED_TEST, __imp_f
  testq %rcx, %rcx

.ifdef READ
// A fused read is a 64-bit load of the pointer itself through RIP.
// READ-COUNT-5: error: fused read is not a 64-bit mov of a pointer
  movl __imp_f@FUSED(%rip), %eax
  leaq __imp_f@FUSED(%rip), %rax
  movq %rax, __imp_f@FUSED(%rip)
  movq __imp_f@FUSED+8(%rip), %rax
  movq __imp_f@FUSED(%rcx), %rax
.endif

.ifdef ERR
// A fused call is six bytes: a call through a register and the CS overrides
// before it, which begin where its marker is.
// ERR-COUNT-10: error: fused call does not name a pointer at a six-byte call through a register
  .reloc ., IMAGE_REL_LLVM_FUSED_CALL, __imp_f
  callq *%rax
  .reloc ., IMAGE_REL_LLVM_FUSED_CALL, __imp_f
  cs; cs; cs
  callq *%rax
  .reloc ., IMAGE_REL_LLVM_FUSED_CALL, __imp_f
  cs; cs; cs; cs
  callq *%r8
  .reloc ., IMAGE_REL_LLVM_FUSED_CALL, __imp_f
  cs; cs; cs; cs
  callq *__imp_f(%rip)
  .reloc ., IMAGE_REL_LLVM_FUSED_CALL, __imp_f
  cs; cs
  .byte 0x2e, 0x2e
  callq *%rax
  cs
  .reloc ., IMAGE_REL_LLVM_FUSED_CALL, __imp_f
  cs; cs; cs
  callq *%rax
  .reloc .+1, IMAGE_REL_LLVM_FUSED_CALL, __imp_f
  cs; cs; cs; cs
  callq *%rax
  .reloc ., IMAGE_REL_LLVM_FUSED_CALL, __imp_f+8
  cs; cs; cs; cs
  callq *%rax
  .reloc ., IMAGE_REL_LLVM_FUSED_CALL, __imp_f
  .reloc ., IMAGE_REL_LLVM_FUSED_CALL, __imp_f
  cs; cs; cs; cs
  callq *%rax
  .reloc ., IMAGE_REL_LLVM_FUSED_CALL, __imp_f
  rex64 jmpq *%rax
  int3; int3; int3; int3

// A fused jump is seven bytes: a jmp with REX.W through a register, where
// its marker is, and the int3 after it.
// ERR-COUNT-4: error: fused jump does not name a pointer at a seven-byte jmp through a register
  .reloc ., IMAGE_REL_LLVM_FUSED_JUMP, __imp_f
  jmpq *%rax
  .reloc ., IMAGE_REL_LLVM_FUSED_JUMP, __imp_f
  cs; cs; cs; cs
  callq *%rax
  .reloc ., IMAGE_REL_LLVM_FUSED_JUMP, __imp_f
  rex64 jmpq *%rax
  int3; int3; int3
  retq
  .reloc ., IMAGE_REL_LLVM_FUSED_JUMP, __imp_f
  rex64 jmpq *%rax
  int3; int3
  .byte 0xcc
  int3; int3

// The register it calls through holds what a fused read of its pointer
// loaded: not what another instruction wrote, also to a part of it, what a
// load that is no fused read loaded, or another pointer.
// ERR-COUNT-4: error: fused call reads RAX, which no fused read of '__imp_f' last wrote
// ERR:      error: fused jump reads RAX, which no fused read of '__imp_f' last wrote
  movq __imp_f@FUSED(%rip), %rax
  movq %rcx, %rax
  .reloc ., IMAGE_REL_LLVM_FUSED_CALL, __imp_f
  cs; cs; cs; cs
  callq *%rax
  movq __imp_f@FUSED(%rip), %rax
  movb $1, %al
  .reloc ., IMAGE_REL_LLVM_FUSED_CALL, __imp_f
  cs; cs; cs; cs
  callq *%rax
  movq __imp_f(%rip), %rax
  .reloc ., IMAGE_REL_LLVM_FUSED_CALL, __imp_f
  cs; cs; cs; cs
  callq *%rax
  movq __imp_g@FUSED(%rip), %rax
  .reloc ., IMAGE_REL_LLVM_FUSED_CALL, __imp_f
  cs; cs; cs; cs
  callq *%rax
  movq __imp_g@FUSED(%rip), %rax
  .reloc ., IMAGE_REL_LLVM_FUSED_JUMP, __imp_f
  rex64 jmpq *%rax
  int3; int3; int3; int3

// A change of section ends what the assembler sees, as a call does.
  movq __imp_g@FUSED(%rip), %rax
  .section .text$other,"xr"
  .reloc ., IMAGE_REL_LLVM_FUSED_CALL, __imp_f
  cs; cs; cs; cs
  callq *%rax
.endif

.ifdef END
// END: [[#@LINE+1]]:{{[0-9]+}}: error: fused jump does not name a pointer at a seven-byte jmp through a register
  .reloc ., IMAGE_REL_LLVM_FUSED_JUMP, __imp_f
  rex64 jmpq *%rax
  int3; int3; int3
.endif

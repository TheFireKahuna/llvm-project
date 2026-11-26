// DEFINE: %{mc} = llvm-mc -mattr=+egpr
// RUN: %{mc} -triple x86_64-unknown-windows-itanium -filetype=obj %s -o %t.o
// RUN: llvm-readobj -r --coff-link-records %t.o | FileCheck %s
// RUN: llvm-objdump -d -j .text %t.o | FileCheck %s --check-prefix=DIS
// RUN: %{mc} -triple x86_64-pc-windows-ntposix %s | \
// RUN:   %{mc} -triple x86_64-pc-windows-ntposix -filetype=obj -o %t.s.o
// RUN: llvm-readobj -r --coff-link-records %t.s.o | FileCheck %s
// RUN: not %{mc} -triple x86_64-unknown-windows-itanium -filetype=obj \
// RUN:   -defsym=ERR=1 %s -o /dev/null 2>&1 | \
// RUN:   FileCheck %s --check-prefix=ERR --implicit-check-not=error:
// RUN: not %{mc} -triple x86_64-unknown-windows-itanium -filetype=obj \
// RUN:   -defsym=END=1 %s -o /dev/null 2>&1 | \
// RUN:   FileCheck %s --check-prefix=END --implicit-check-not=error:
// RUN: not %{mc} -triple x86_64-pc-windows-msvc -filetype=obj %s \
// RUN:   -o /dev/null 2>&1 | FileCheck %s --check-prefix=MSVC
// RUN: %{mc} -triple x86_64-unknown-windows-itanium -filetype=obj \
// RUN:   -x86-align-branch-boundary=32 -x86-align-branch=call %s -o %t.b.o
// RUN: llvm-objdump -d -j '.text$b' %t.b.o | FileCheck %s --check-prefix=PAD

// On Windows Itanium and NT-POSIX @LOAD_CALL makes a load and the call through
// its register one site, MEM_LOAD_CALL at the load's empty field, which the
// linker may rewrite as a five-byte direct call: a jump across it is a span,
// or a relocation where a fused read makes the linker move the code.

// CHECK:      Section ({{[0-9]+}}) .text {
// CHECK-NEXT:   0x5 IMAGE_REL_LLVM_MEM_LOAD_CALL __imp_f
// CHECK-NEXT:   0xE IMAGE_REL_LLVM_MEM_LOAD_CALL __imp_f
// CHECK-NEXT: }
// CHECK-NEXT: Section ({{[0-9]+}}) .text$m {
// CHECK-NEXT:   0x1 IMAGE_REL_LLVM_JUMP8
// CHECK-NEXT:   0x5 IMAGE_REL_LLVM_MEM_LOAD_CALL __imp_f
// CHECK-NEXT:   0xE IMAGE_REL_LLVM_FUSED_READ __imp_f
// CHECK-NEXT: }
// CHECK:      ResizableSection: .text (
// CHECK-NEXT: ResizableSection: .text$m (
// CHECK:      Kind: Spans (0x5)
// CHECK-NEXT: Spans {
// CHECK-NEXT:   Field: .text (
// CHECK-NEXT:   Target: .text (
// CHECK-NEXT:   0x1 REL8
// CHECK-NEXT: }
// CHECK-NEXT: Spans {
// CHECK-NEXT:   Field: .text$b (
// CHECK-NEXT:   Target: .text$b (
// CHECK-NEXT: }
// CHECK-NEXT: }

// DIS:      eb 13                  jmp
// DIS-NEXT: 48 8b 05 00 00 00 00   movq
// DIS-NEXT: ff d0                  callq *%rax
// DIS-NEXT: 4c 8b 1d 00 00 00 00   movq
// DIS-NEXT: 41 ff d3               callq *%r11

// The two are one unit to the padding that keeps a call within a boundary,
// which goes before the load.
// PAD:      19: 0f 1f 80 00 00 00 00   nopl
// PAD-NEXT: 20: 48 8b 05 00 00 00 00   movq
// PAD-NEXT: 27: ff d0                  callq *%rax

// MSVC: [[#@LINE+5]]:{{[0-9]+}}: error: relocation specifier @LOAD_CALL unsupported on COFF targets

  .text
  jmp 1f
.ifndef ERR
  movq __imp_f@LOAD_CALL(%rip), %rax
  callq *%rax
  movq __imp_f@LOAD_CALL(%rip), %r11
  callq *%r11
1:
  retq

  .section .text$m,"xr"
  jmp 1f
  movq __imp_f@LOAD_CALL(%rip), %rax
  callq *%rax
  movq __imp_f@FUSED(%rip), %rax
1:
  retq

  .section .text$b,"xr"
  .rept 25
  nop
  .endr
  movq __imp_f@LOAD_CALL(%rip), %rax
  callq *%rax

.else
// The call is through the register the load wrote, directly after it.
// ERR-COUNT-5: error: load of a pointer for a call is not followed by the call through its register
// ERR: error: label between the load of a pointer for a call and the call
  movq __imp_f@LOAD_CALL(%rip), %rax
  callq *%rcx
  movq __imp_f@LOAD_CALL(%rip), %rax
  nop
  callq *%rax
  movq __imp_f@LOAD_CALL(%rip), %rax
  .byte 0x90
  callq *%rax
  movq __imp_f@LOAD_CALL(%rip), %rax
  jmpq *%rax
  movq __imp_f@LOAD_CALL(%rip), %rax
  notrack callq *%rax
  movq __imp_f@LOAD_CALL(%rip), %rax
2:
  callq *%rax
// The load is a mov of 64 bits through RIP with a REX prefix, of the pointer
// itself.
// ERR-COUNT-4: error: @LOAD_CALL is not on a 64-bit mov of a pointer into one of the first sixteen registers
  movq __imp_f@LOAD_CALL(%rip), %r16
  callq *%r16
  movq __imp_f@LOAD_CALL+8(%rip), %rax
  callq *%rax
  movq %fs:__imp_f@LOAD_CALL(%rip), %rax
  callq *%rax
  callq *__imp_f@LOAD_CALL(%rip)
1:
.endif

.ifdef END
// END: error: load of a pointer for a call is not followed by the call through its register
  movq __imp_f@LOAD_CALL(%rip), %rax
.endif

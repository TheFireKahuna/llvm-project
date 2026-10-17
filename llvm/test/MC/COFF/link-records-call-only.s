// A frame's handler, which the system only calls, is listed as a call-only
// reference in a LinkRecordCallOnly group (kind 4), on x86-64 and AArch64, on
// Windows Itanium; other environments write no records.

// RUN: llvm-mc -triple x86_64-unknown-windows-itanium -filetype=obj %s -o %t.o
// RUN: llvm-objdump -s -j .llvm_link_records %t.o | \
// RUN:   FileCheck --check-prefix=X86 %s
// RUN: llvm-objdump -r -j .xdata %t.o | FileCheck --check-prefix=RELOC %s
// RUN: llvm-mc -triple x86_64-pc-windows-msvc -filetype=obj %s -o %t.msvc.o
// RUN: llvm-objdump -h %t.msvc.o | FileCheck --check-prefix=NONE %s

// "LLRC", version 1, the x86-64 sites and call-only capabilities (3), then a
// group of kind 4 and 3 bytes: .xdata's symbol (index 6), one reference, at
// offset 8.
// X86:      Contents of section .llvm_link_records:
// X86-NEXT: 0000 4c4c5243 01030403 060108
// RELOC:    0000000000000008 IMAGE_REL_AMD64_ADDR32NB __gxx_personality_seh0
// NONE-NOT: .llvm_link_records

  .text
  .def f; .scl 2; .type 32; .endef
  .globl f
f:
  .seh_proc f
  .seh_handler __gxx_personality_seh0, @unwind, @except
  pushq %rbp
  .seh_pushreg %rbp
  .seh_endprologue
  popq %rbp
  retq
  .seh_endproc

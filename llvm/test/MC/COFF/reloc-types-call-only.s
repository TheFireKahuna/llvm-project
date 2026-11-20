// REQUIRES: aarch64-registered-target
// RUN: llvm-mc -triple x86_64-unknown-windows-itanium -filetype=obj \
// RUN:   -defsym=X64=1 %s -o %t.x64.o
// RUN: llvm-readobj -r -x .data %t.x64.o | \
// RUN:   FileCheck %s --check-prefixes=CHECK,X64 -DM=AMD64
// RUN: llvm-mc -triple aarch64-pc-windows-ntposix -filetype=obj %s -o %t.a64.o
// RUN: llvm-readobj -r -x .data %t.a64.o | \
// RUN:   FileCheck %s --check-prefixes=CHECK,A64 -DM=ARM64
// RUN: llvm-mc -triple x86_64-pc-windows-msvc -filetype=obj -defsym=X64=1 %s \
// RUN:   -o %t.m.o
// RUN: llvm-readobj -r %t.m.o | FileCheck %s --check-prefix=MSVC
// RUN: llvm-mc -triple aarch64-pc-windows-msvc -filetype=obj %s -o %t.ma.o
// RUN: llvm-readobj -r %t.ma.o | FileCheck %s --check-prefix=MSVC

// On Windows Itanium and NT-POSIX a data field through which a function is
// only called states so in its relocation type: a frame's handler, which the
// system calls, and a field with @IMGREL_CALL are ADDR32NB_CALL, and a field
// with @REL32_CALL, or on x86-64 a PC-relative one with @PLT, is REL32_CALL,
// its addend in the field as REL32's is. Elsewhere the two are ADDR32NB and
// REL32.

// MSVC-NOT: IMAGE_REL_LLVM
// MSVC:      Section ({{[0-9]+}}) .data {
// MSVC-NEXT:   0x0 IMAGE_REL_{{AMD|ARM}}64_ADDR32NB g
// MSVC-NEXT:   0x4 IMAGE_REL_{{AMD|ARM}}64_REL32 g
// MSVC-NEXT:   0x8 IMAGE_REL_{{AMD|ARM}}64_REL32 g
// MSVC-NOT: IMAGE_REL_LLVM

// CHECK:      Section ({{[0-9]+}}) .data {
// CHECK-NEXT:   0x0 IMAGE_REL_LLVM_ADDR32NB_CALL g
// CHECK-NEXT:   0x4 IMAGE_REL_LLVM_REL32_CALL g
// CHECK-NEXT:   0x8 IMAGE_REL_LLVM_REL32_CALL g
// X64-NEXT:     0xC IMAGE_REL_AMD64_REL32 g
// X64-NEXT:     0x10 IMAGE_REL_LLVM_REL32_CALL g
// X64-NEXT:     0x14 IMAGE_REL_LLVM_REL32_CALL g
// X64-NEXT:     0x18 IMAGE_REL_AMD64_ADDR32NB g
// CHECK-NEXT: }
// CHECK:      Section ({{[0-9]+}}) .xdata {
// CHECK-NEXT:   {{0x[48]}} IMAGE_REL_LLVM_ADDR32NB_CALL __gxx_personality_seh0
// CHECK-NEXT: }
// X64:      0x00000000 00000000 04000000 0c000000 04000000
// X64-NEXT: 0x00000010 04000000 0c000000 00000000
// A64:      0x00000000 00000000 04000000 0c000000

  .text
  .def f; .scl 2; .type 32; .endef
  .globl f
f:
  .seh_proc f
  .seh_handler __gxx_personality_seh0, @unwind, @except
  .seh_endprologue
  ret
  .seh_endproc

  .data
  .long g@IMGREL_CALL
  .long g@REL32_CALL
  .long g@REL32_CALL + 8
.ifdef X64
  .long g - .
  .long g@PLT - .
  .long g@PLT - . + 8
  .long g@IMGREL
.endif

// RUN: rm -rf %t && split-file %s %t
// RUN: llvm-mc -triple aarch64-unknown-windows-itanium -filetype=obj %t/a.s \
// RUN:   -o %t/a.o
// RUN: llvm-readobj -r %t/a.o | FileCheck %s --check-prefix=ASM
// RUN: llvm-mc -triple aarch64-pc-windows-ntposix -filetype=obj %t/a.s \
// RUN:   -o %t/a.n.o
// RUN: llvm-readobj -r %t/a.n.o | FileCheck %s --check-prefix=ASM
// RUN: llvm-mc -triple aarch64-pc-windows-msvc -filetype=obj %t/a.s \
// RUN:   -o %t/a.m.o
// RUN: llvm-readobj -r %t/a.m.o | \
// RUN:   FileCheck %s --check-prefix=MSVC --implicit-check-not=IMAGE_REL_LLVM
// RUN: llc -mtriple=aarch64-unknown-windows-itanium -filetype=obj %t/b.ll \
// RUN:   -o %t/b.o
// RUN: llvm-readobj -r %t/b.o | FileCheck %s --check-prefix=IR
// RUN: llc -mtriple=aarch64-pc-windows-msvc -filetype=obj %t/b.ll -o %t/b.m.o
// RUN: llvm-readobj -r %t/b.m.o | \
// RUN:   FileCheck %s --check-prefix=MSVC --implicit-check-not=IMAGE_REL_LLVM

// On Windows Itanium and NT-POSIX a b assembled with no unwind frame open is
// BRANCH_TAIL, which the linker may delete. In a frame it is BRANCH26, as bl
// is anywhere. A conditional branch keeps its type, and a b to a label in
// its section needs no relocation.

// MSVC: IMAGE_REL_ARM64_BRANCH26 {{ext|callee}}

// ASM:      Section ({{[0-9]+}}) .text {
// ASM-NEXT:   0x0 IMAGE_REL_LLVM_BRANCH_TAIL ext
// ASM-NEXT:   0x4 IMAGE_REL_ARM64_BRANCH19 ext
// ASM-NEXT:   0x8 IMAGE_REL_ARM64_BRANCH14 ext
// ASM-NEXT:   0xC IMAGE_REL_ARM64_BRANCH26 ext
// ASM-NEXT:   0x1C IMAGE_REL_ARM64_BRANCH19 ext
// ASM-NEXT:   0x28 IMAGE_REL_ARM64_BRANCH26 ext
// ASM-NEXT:   0x2C IMAGE_REL_LLVM_BRANCH_TAIL ext
// ASM-NEXT: }

// IR:      Section ({{[0-9]+}}) .text {
// IR-NEXT:   0x0 IMAGE_REL_LLVM_BRANCH_TAIL callee
// IR-NEXT:   0x10 IMAGE_REL_ARM64_BRANCH26 other
// IR-NEXT:   0x20 IMAGE_REL_ARM64_BRANCH26 callee
// IR-NEXT: }

//--- a.s
  .text
  .globl frameless
frameless:
  b ext
  b.ne ext
  tbz x0, #1, ext
  bl ext
  b .Lnear
.Lnear:
  ret

  .globl framed
  .seh_proc framed
framed:
  sub sp, sp, #16
  .seh_stackalloc 16
  .seh_endprologue
  b.ne ext
  b .Lframed
.Lframed:
  .seh_startepilogue
  add sp, sp, #16
  .seh_stackalloc 16
  .seh_endepilogue
  b ext
  .seh_endproc
  b ext

//--- b.ll
declare i32 @callee(i32)
declare void @other()

define i32 @frameless(i32 %x) nounwind {
  %r = tail call i32 @callee(i32 %x)
  ret i32 %r
}

define i32 @framed(i32 %x) {
  call void @other()
  %r = tail call i32 @callee(i32 %x)
  ret i32 %r
}

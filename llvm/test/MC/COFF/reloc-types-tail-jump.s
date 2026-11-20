// RUN: rm -rf %t && split-file %s %t
// RUN: llvm-mc -triple x86_64-unknown-windows-itanium -filetype=obj %t/a.s \
// RUN:   -o %t/a.o
// RUN: llvm-readobj -r %t/a.o | FileCheck %s --check-prefix=ASM
// RUN: llvm-mc -triple x86_64-pc-windows-ntposix -filetype=obj %t/a.s \
// RUN:   -o %t/a.n.o
// RUN: llvm-readobj -r %t/a.n.o | FileCheck %s --check-prefix=ASM
// RUN: llvm-mc -triple x86_64-pc-windows-msvc -filetype=obj %t/a.s \
// RUN:   -o %t/a.m.o
// RUN: llvm-readobj -r %t/a.m.o | \
// RUN:   FileCheck %s --check-prefix=MSVC --implicit-check-not=IMAGE_REL_LLVM
// RUN: llc -mtriple=x86_64-unknown-windows-itanium -filetype=obj %t/b.ll \
// RUN:   -o %t/b.o
// RUN: llvm-readobj -r %t/b.o | FileCheck %s --check-prefix=IR
// RUN: llc -mtriple=x86_64-pc-windows-msvc -filetype=obj %t/b.ll -o %t/b.m.o
// RUN: llvm-readobj -r %t/b.m.o | \
// RUN:   FileCheck %s --check-prefix=MSVC --implicit-check-not=IMAGE_REL_LLVM

// On Windows Itanium and NT-POSIX the relocation type of a jmp follows the
// unwind frame open where it is assembled. With no frame open a jmp the
// assembler relaxed to 32 bits is BRANCH_TAIL, which the linker may delete. In
// a frame it is JUMP32, and under version 2 unwind data a jmp at an epilogue's
// end is BRANCH32, since the epilogue's stated size counts it. A jmp written
// with 32 bits is BRANCH32 and a jcc JUMP32 everywhere, and a jmp that stays
// short needs no relocation.

// MSVC: IMAGE_REL_AMD64_REL32 {{ext|callee}}

// ASM:      Section ({{[0-9]+}}) .text {
// ASM-NEXT:   0x1 IMAGE_REL_LLVM_BRANCH_TAIL ext
// ASM-NEXT:   0x7 IMAGE_REL_LLVM_JUMP32 ext
// ASM-NEXT:   0xE IMAGE_REL_LLVM_BRANCH32 ext
// ASM-NEXT:   0x19 IMAGE_REL_LLVM_JUMP32 ext
// ASM-NEXT:   0x24 IMAGE_REL_LLVM_JUMP32 ext
// ASM-NEXT:   0x2D IMAGE_REL_LLVM_JUMP32 ext
// ASM-NEXT:   0x36 IMAGE_REL_LLVM_BRANCH32 ext
// ASM-NEXT:   0x3B IMAGE_REL_LLVM_BRANCH_TAIL ext
// ASM-NEXT: }

// IR:      Section ({{[0-9]+}}) .text {
// IR-NEXT:   0xA IMAGE_REL_LLVM_BRANCH_TAIL callee
// IR-NEXT:   0xF IMAGE_REL_LLVM_BRANCH_TAIL callee
// IR-NEXT:   0x28 IMAGE_REL_LLVM_BRANCH32 other
// IR-NEXT:   0x34 IMAGE_REL_LLVM_JUMP32 callee
// IR-NEXT: }

//--- a.s
  .text
  .globl frameless
frameless:
  jmp ext
  jne ext
  jmp .Lnear
  {disp32} jmp ext
.Lnear:
  retq

  .globl framed
  .seh_proc framed
framed:
  subq $40, %rsp
  .seh_stackalloc 40
  .seh_endprologue
  jne ext
  jmp .Lframed
.Lframed:
  .seh_startepilogue
  addq $40, %rsp
  .seh_endepilogue
  jmp ext
  .seh_endproc

// Version 2: a jmp in the body, one at the epilogue's end, and one after the
// frame.
  .globl v2
  .seh_proc v2
v2:
  .seh_unwindversion 2
  subq $40, %rsp
  .seh_stackalloc 40
  .seh_endprologue
  jmp ext
  .seh_startepilogue
  addq $40, %rsp
  .seh_unwindv2start
  .seh_endepilogue
  jmp ext
  .seh_endproc
  jmp ext

//--- b.ll
declare i32 @callee(i32)
declare void @other()

; The je to %call is short. Each function ends in a jmp to callee.
define i32 @frameless(i32 %x) nounwind {
  %c = icmp eq i32 %x, 0
  br i1 %c, label %call, label %ret
call:
  %r = tail call i32 @callee(i32 %x)
  ret i32 %r
ret:
  %s = tail call i32 @callee(i32 1)
  ret i32 %s
}

define i32 @framed(i32 %x) {
  call void @other()
  %r = tail call i32 @callee(i32 %x)
  ret i32 %r
}

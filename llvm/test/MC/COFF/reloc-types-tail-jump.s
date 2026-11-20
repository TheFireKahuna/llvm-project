// REQUIRES: aarch64-registered-target
// RUN: rm -rf %t && split-file %s %t
// RUN: llvm-mc -triple x86_64-unknown-windows-itanium -filetype=obj %t/a.s \
// RUN:   -o %t/a.o
// RUN: llvm-readobj -r %t/a.o | FileCheck %s --check-prefix=ASM
// RUN: llvm-mc -triple x86_64-pc-windows-ntposix -filetype=obj %t/a.s \
// RUN:   -o %t/a.n.o
// RUN: llvm-readobj -r %t/a.n.o | FileCheck %s --check-prefix=ASM
// RUN: llvm-mc -triple aarch64-pc-windows-ntposix -filetype=obj %t/a64.s \
// RUN:   -o %t/a64.o
// RUN: llvm-readobj -r %t/a64.o | FileCheck %s --check-prefix=A64
// RUN: llc -mtriple=x86_64-unknown-windows-itanium -filetype=obj %t/b.ll \
// RUN:   -o %t/b.o
// RUN: llvm-readobj -r %t/b.o | FileCheck %s --check-prefix=IR
// RUN: llc -mtriple=aarch64-unknown-windows-itanium -filetype=obj %t/b.ll \
// RUN:   -o %t/b64.o
// RUN: llvm-readobj -r %t/b64.o | FileCheck %s --check-prefix=IR64
// RUN: llvm-mc -triple x86_64-pc-windows-msvc -filetype=obj %t/a.s -o %t/m.o
// RUN: llvm-mc -triple aarch64-pc-windows-msvc -filetype=obj %t/a64.s -o %t/m64.o
// RUN: llc -mtriple=x86_64-pc-windows-msvc -filetype=obj %t/b.ll -o %t/bm.o
// RUN: llc -mtriple=aarch64-pc-windows-msvc -filetype=obj %t/b.ll -o %t/bm64.o
// RUN: llvm-readobj -r %t/m.o %t/m64.o %t/bm.o %t/bm64.o | \
// RUN:   FileCheck %s --check-prefix=MSVC --implicit-check-not=IMAGE_REL_LLVM

// On Windows Itanium and NT-POSIX the relocation type of a jmp follows the
// unwind frame open where it is assembled. With no frame open a jmp the
// assembler relaxed to 32 bits is BRANCH_TAIL, which the linker may delete. In
// a frame it is JUMP32, unless it ends an epilogue after which the unwinder
// is right with the jmp gone: any epilogue under version 2 unwind data, and
// under version 1 the stack adjustment of a frame without a push, or a lone
// `leave`. A jmp written with 32 bits is BRANCH32 and a jcc JUMP32 everywhere,
// and a jmp that stays short is JUMP8. On AArch64 a b with no frame open is
// BRANCH_TAIL and one in a frame BRANCH26, as bl is anywhere; a conditional
// branch keeps its type, and a b to a label across code the linker may resize
// keeps its relocation, against a label.

// MSVC-COUNT-2: IMAGE_REL_AMD64_REL32 {{ext|callee}}
// MSVC:         IMAGE_REL_ARM64_BRANCH26 {{ext|callee}}

// ASM:      Section ({{[0-9]+}}) .text {
// ASM-NEXT:   0x1 IMAGE_REL_LLVM_BRANCH_TAIL ext
// ASM-NEXT:   0x7 IMAGE_REL_LLVM_JUMP32 ext
// ASM-NEXT:   0xC IMAGE_REL_LLVM_JUMP8 $L1
// ASM-NEXT:   0xE IMAGE_REL_LLVM_BRANCH32 ext
// ASM-NEXT:   0x19 IMAGE_REL_LLVM_JUMP32 ext
// ASM-NEXT:   0x1E IMAGE_REL_LLVM_JUMP8 $L2
// ASM-NEXT:   0x24 IMAGE_REL_LLVM_BRANCH_TAIL ext
// ASM-NEXT:   0x2D IMAGE_REL_LLVM_JUMP32 ext
// ASM-NEXT:   0x36 IMAGE_REL_LLVM_BRANCH_TAIL ext
// ASM-NEXT:   0x3B IMAGE_REL_LLVM_BRANCH_TAIL ext
// ASM-NEXT:   0x4A IMAGE_REL_LLVM_JUMP32 ext
// ASM-NEXT:   0x54 IMAGE_REL_LLVM_BRANCH_TAIL ext
// ASM-NEXT:   0x62 IMAGE_REL_LLVM_JUMP32 ext
// ASM-NEXT: }

// IR:      Section ({{[0-9]+}}) .text {
// IR-NEXT:   0x3 IMAGE_REL_LLVM_JUMP8 $L1
// IR-NEXT:   0xA IMAGE_REL_LLVM_BRANCH_TAIL callee
// IR-NEXT:   0xF IMAGE_REL_LLVM_BRANCH_TAIL callee
// IR-NEXT:   0x13 IMAGE_REL_LLVM_ALIGN - (324)
// IR-NEXT:   0x28 IMAGE_REL_LLVM_BRANCH32 other
// IR-NEXT:   0x34 IMAGE_REL_LLVM_JUMP32 callee
// IR-NEXT: }

// A64:      Section ({{[0-9]+}}) .text {
// A64-NEXT:   0x0 IMAGE_REL_LLVM_BRANCH_TAIL ext
// A64-NEXT:   0x4 IMAGE_REL_ARM64_BRANCH19 ext
// A64-NEXT:   0x8 IMAGE_REL_ARM64_BRANCH14 ext
// A64-NEXT:   0xC IMAGE_REL_ARM64_BRANCH26 ext
// A64-NEXT:   0x10 IMAGE_REL_LLVM_BRANCH_TAIL $L{{[0-9a-f]+}}
// A64-NEXT:   0x1C IMAGE_REL_ARM64_BRANCH19 ext
// A64-NEXT:   0x28 IMAGE_REL_ARM64_BRANCH26 ext
// A64-NEXT:   0x2C IMAGE_REL_LLVM_BRANCH_TAIL ext
// A64-NEXT: }

// IR64:      Section ({{[0-9]+}}) .text {
// IR64-NEXT:   IMAGE_REL_LLVM_BRANCH_TAIL callee
// IR64-NEXT:   IMAGE_REL_ARM64_BRANCH26 other
// IR64-NEXT:   IMAGE_REL_ARM64_BRANCH26 callee
// IR64-NEXT: }

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

// Version 1: after an epilogue that pops, after a lone `leave`, and after an
// epilogue that reloads a register before its `leave`.
  .seh_proc pushed
pushed:
  pushq %rbx
  .seh_pushreg %rbx
  subq $32, %rsp
  .seh_stackalloc 32
  .seh_endprologue
  .seh_startepilogue
  addq $32, %rsp
  popq %rbx
  .seh_endepilogue
  jmp ext
  .seh_endproc

  .seh_proc left
left:
  pushq %rbp
  .seh_pushreg %rbp
  movq %rsp, %rbp
  .seh_setframe %rbp, 0
  .seh_endprologue
  .seh_startepilogue
  leave
  .seh_endepilogue
  jmp ext
  .seh_endproc

  .seh_proc reloaded
reloaded:
  pushq %rbp
  .seh_pushreg %rbp
  movq %rsp, %rbp
  .seh_setframe %rbp, 0
  .seh_endprologue
  .seh_startepilogue
  movq -8(%rbp), %rbx
  leave
  .seh_endepilogue
  jmp ext
  .seh_endproc

//--- a64.s
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

// RUN: rm -rf %t && split-file %s %t
// RUN: llvm-mc -triple x86_64-unknown-windows-itanium -filetype=obj %t/a.s \
// RUN:   -o %t/a.o
// RUN: llvm-readobj -r %t/a.o | FileCheck %s --check-prefix=ASM
// RUN: llvm-objdump -d -j '.text$over' %t/a.o | \
// RUN:   FileCheck %s --check-prefix=OVER
// RUN: llvm-mc -triple x86_64-pc-windows-ntposix -filetype=obj %t/a.s \
// RUN:   -o %t/a.n.o
// RUN: llvm-readobj -r %t/a.n.o | FileCheck %s --check-prefix=ASM
// RUN: llvm-mc -triple x86_64-pc-windows-msvc -filetype=obj --defsym MSVC=1 \
// RUN:   %t/a.s -o %t/m.o
// RUN: llvm-readobj -r %t/m.o | \
// RUN:   FileCheck %s --check-prefix=MSVC --implicit-check-not=IMAGE_REL_LLVM
// RUN: %if aarch64-registered-target %{ llvm-mc -triple aarch64-pc-windows-ntposix \
// RUN:   -filetype=obj %t/a64.s -o %t/a64.o %}
// RUN: %if aarch64-registered-target %{ llvm-readobj -r %t/a64.o | \
// RUN:   FileCheck %s --check-prefix=A64 %}
// RUN: %if aarch64-registered-target %{ llvm-mc -triple aarch64-pc-windows-msvc \
// RUN:   -filetype=obj --defsym MSVC=1 %t/a64.s -o %t/m64.o %}
// RUN: %if aarch64-registered-target %{ llvm-readobj -r %t/m64.o | \
// RUN:   FileCheck %s --check-prefix=MSVC64 --implicit-check-not=IMAGE_REL_LLVM %}

// elsewhere; after a prefix byte it is MEM_JUMP_P1. On AArch64 a b with no
// frame open is BRANCH_TAIL and one in a frame BRANCH26, as bl is anywhere,
// and a fused br, whose marker states either type, FUSED_JUMP_TAIL and
// FUSED_JUMP; a conditional branch keeps its type, and a b to a label across
// code the linker may resize keeps its relocation, against a label.

// MSVC:   IMAGE_REL_AMD64_REL32 ext
// MSVC64: IMAGE_REL_ARM64_BRANCH26 ext

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
// ASM-NEXT:   0x{{[0-9A-F]+}} IMAGE_REL_LLVM_FUSED_READ __imp_g
// ASM-NEXT: }
// ASM:      Section ({{[0-9]+}}) .text$mem {
// ASM-NEXT:   0x2 IMAGE_REL_LLVM_MEM_JUMP_TAIL __imp_g
// ASM-NEXT:   0x9 IMAGE_REL_LLVM_MEM_JUMP_P1 __imp_g
// ASM-NEXT:   0x13 IMAGE_REL_LLVM_MEM_JUMP __imp_g
// ASM-NEXT:   0x1D IMAGE_REL_LLVM_MEM_JUMP_TAIL __imp_g
// ASM-NEXT:   0x25 IMAGE_REL_LLVM_MEM_JUMP __imp_g
// ASM-NEXT: }

// A jcc that jumps over a jmp through a pointer, where no frame is open, to
// the label directly after it is a conditional tail call the other way
// round. Bit 0 of the jmp's field says so, and that nothing else reaches the
// jmp: the linker writes the two as one jcc where it binds the pointer. A
// label at the jmp, a jcc that goes elsewhere and one that is not directly
// before the jmp leave the field clear.
// OVER:      74 06                je
// OVER-NEXT: ff 25 01 00 00 00    jmpq
// OVER-NEXT: 74 06                je
// OVER-NEXT: ff 25 00 00 00 00    jmpq
// OVER-NEXT: 74 f8                je
// OVER-NEXT: ff 25 00 00 00 00    jmpq
// OVER-NEXT: 74 07                je
// OVER-NEXT: 90                   nop
// OVER-NEXT: ff 25 00 00 00 00    jmpq

// A64:      Section ({{[0-9]+}}) .text {
// A64-NEXT:   0x0 IMAGE_REL_LLVM_BRANCH_TAIL ext
// A64-NEXT:   0x4 IMAGE_REL_ARM64_BRANCH19 ext
// A64-NEXT:   0x8 IMAGE_REL_ARM64_BRANCH14 ext
// A64-NEXT:   0xC IMAGE_REL_ARM64_BRANCH26 ext
// A64-NEXT:   0x10 IMAGE_REL_LLVM_BRANCH_TAIL $L{{[0-9a-f]+}}
// A64-NEXT:   0x1C IMAGE_REL_ARM64_BRANCH19 ext
// A64-NEXT:   0x28 IMAGE_REL_ARM64_BRANCH26 ext
// A64-NEXT:   0x2C IMAGE_REL_LLVM_BRANCH_TAIL ext
// A64-NEXT:   0x30 IMAGE_REL_LLVM_FUSED_JUMP_TAIL __imp_g
// A64-NEXT:   0x38 IMAGE_REL_LLVM_FUSED_JUMP __imp_g
// A64-NEXT: }

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

// A jmp through a pointer with no prefix follows the same frames.
// A jmp that stays short has a relocation where the linker moves the code of
// its section, as it does after a fused read.
.ifndef MSVC
  movq __imp_g@FUSED(%rip), %rax
.endif

  .section .text$mem,"xr"
  jmpq *__imp_g(%rip)
  rex64 jmpq *__imp_g(%rip)
  .seh_proc memframed
memframed:
  subq $40, %rsp
  .seh_stackalloc 40
  .seh_endprologue
  jmpq *__imp_g(%rip)
  .seh_startepilogue
  addq $40, %rsp
  .seh_endepilogue
  jmpq *__imp_g(%rip)
  .seh_endproc
  .seh_proc mempushed
mempushed:
  pushq %rbx
  .seh_pushreg %rbx
  .seh_endprologue
  .seh_startepilogue
  popq %rbx
  .seh_endepilogue
  jmpq *__imp_g(%rip)
  .seh_endproc

  .section .text$over,"xr"
  je 1f
  jmpq *__imp_g(%rip)
1:
  je 1f
2:
  jmpq *__imp_g(%rip)
1:
  je 2b
  jmpq *__imp_g(%rip)
  je 1f
  nop
  jmpq *__imp_g(%rip)
1:
  retq

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
.ifndef MSVC
  .reloc ., IMAGE_REL_LLVM_FUSED_JUMP, __imp_g
  br x16
  .seh_proc fusedframed
fusedframed:
  sub sp, sp, #16
  .seh_stackalloc 16
  .seh_endprologue
  .reloc ., IMAGE_REL_LLVM_FUSED_JUMP_TAIL, __imp_g
  br x16
  .seh_startepilogue
  add sp, sp, #16
  .seh_stackalloc 16
  .seh_endepilogue
  ret
  .seh_endproc
.endif

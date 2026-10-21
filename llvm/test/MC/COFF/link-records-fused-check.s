// A fact that names a load and the register it fills is checked against the
// instructions it describes: the load must fill the register, the instruction
// must read it, and no instruction from which execution falls through to that
// one may write it after the load. What a call, a jump or a trap is followed
// by is reached from code the assembler cannot see. A copy carries the load's
// value to an access or an index, but not to a call, an add or an anchor.

// RUN: not llvm-mc -triple x86_64-unknown-windows-itanium -filetype=obj %s \
// RUN:   -o /dev/null 2>&1 | FileCheck %s --implicit-check-not=error:
// RUN: not llvm-mc -triple x86_64-unknown-windows-itanium -filetype=obj \
// RUN:   -defsym=LOAD=1 %s -o /dev/null 2>&1 | \
// RUN:   FileCheck %s --check-prefix=LOAD --implicit-check-not=error:

  .text
f:
.ifndef LOAD
.La:
  movq __imp_g(%rip), %rax
  xorl %eax, %eax
// CHECK: [[#@LINE+1]]:3: error: fused instruction reads RAX, which an instruction other than the load at '.La' last wrote
  .linkfusedcall .La, %rax
  cs; cs; cs; cs
  callq *%rax

.Lb:
  movq __imp_g(%rip), %rax
  movb $1, %al
// CHECK: [[#@LINE+1]]:3: error: fused instruction reads RAX, which an instruction other than the load at '.Lb' last wrote
  .linkfusedcall .Lb, %rax
  cs; cs; cs; cs
  callq *%rax

.Lc:
  movq __imp_g(%rip), %rax
  movq %rax, %rcx
// CHECK: [[#@LINE+1]]:3: error: fused instruction reads RCX, which a copy last wrote
  .linkfusedcall .Lc, %rcx
  cs; cs; cs; cs
  callq *%rcx

.Ld:
  movq __imp_v(%rip), %rax
  movq %rax, %rcx
  .linkfusedaccess .Ld, %rcx
  movl (%rcx), %edx
  .linkfusedaccess .Ld, %rax
// CHECK: [[#@LINE+1]]:3: error: fused instruction does not read RAX
  movl (%rcx), %edx

.Le:
  movq __imp_v(%rip), %rbx
  xorl %ebx, %ebx
  callq g
  .linkfusedaccess .Le, %rbx
  movl (%rbx), %edx
  callq g
  movl $1, %ebx
// CHECK: [[#@LINE+1]]:3: error: fused instruction reads RBX, which an instruction other than the load at '.Le' last wrote
  .linkfusedaccess .Le, %rbx
  movl (%rbx), %edx

.Lf:
  movq __imp_a(%rip), %rcx
  movq __imp_b(%rip), %rcx
// CHECK: [[#@LINE+1]]:3: error: fused instruction reads RCX, which an instruction other than the load at '.Lf' last wrote
  .linkanchor .Lf, %rcx
  movq __imp_c(%rip), %rdx
  retq
.else
.Lg:
  movq __imp_g(%rip), %rax
// LOAD: [[#@LINE+1]]:3: error: fused instruction reads RBX, which the load at '.Lg' does not fill
  .linkfusedcall .Lg, %rbx
  cs; cs; cs; cs
  callq *%rbx

.Lh:
  leaq v(%rip), %rax
// LOAD: [[#@LINE+1]]:3: error: fused instruction reads the instruction at '.Lh', which is not a load
  .linkfusedadd .Lh, %rax
  addq $16, %rax
  retq

// LOAD: [[#@LINE+1]]:3: error: fused instruction reads RSI, which the load at '.Li' does not fill
  .linkfusedcall .Li, %rsi
  cs; cs; cs; cs
  callq *%rsi
.Li:
  movq __imp_g(%rip), %rdi
  retq
.endif

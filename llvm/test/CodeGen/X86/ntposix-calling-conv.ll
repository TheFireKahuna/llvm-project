; RUN: llc -mtriple=x86_64-pc-windows-ntposix < %s | FileCheck %s
; RUN: llc -mtriple=x86_64-unknown-windows-itanium < %s | \
; RUN:   FileCheck %s --check-prefix=WIN64

; NT-POSIX uses the System V AMD64 convention by default, with Windows x64
; unwind data: integer arguments in RDI, RSI, RDX, RCX, R8 and R9, no shadow
; space, RSI, RDI and XMM6-XMM15 not preserved, and the System V va_list. The
; other Windows environments use the Microsoft x64 convention.

declare void @callee(i64, i64, i64, i64, i64, i64, i64)
declare void @clobber()
declare void @use(ptr)
declare void @llvm.va_start.p0(ptr)
declare void @llvm.va_end.p0(ptr)

define i64 @args(i64 %a, i64 %b, i64 %c, i64 %d, i64 %e, i64 %f, i64 %g) {
; CHECK-LABEL: args:
; CHECK:       leaq (%rdi,%r9), %rax
; CHECK-NEXT:  addq 8(%rsp), %rax
; CHECK-NEXT:  retq
; WIN64-LABEL: args:
; WIN64:       movq %rcx, %rax
; WIN64-NEXT:  addq 48(%rsp), %rax
; WIN64-NEXT:  addq 56(%rsp), %rax
  %s1 = add i64 %a, %f
  %s2 = add i64 %s1, %g
  ret i64 %s2
}

define void @call() {
; CHECK-LABEL: call:
; CHECK:       .seh_endprologue
; CHECK-NEXT:  movq $7, (%rsp)
; CHECK-NEXT:  movl $1, %edi
; CHECK-NEXT:  movl $2, %esi
; CHECK-NEXT:  movl $3, %edx
; CHECK-NEXT:  movl $4, %ecx
; CHECK-NEXT:  movl $5, %r8d
; CHECK-NEXT:  movl $6, %r9d
; CHECK-NEXT:  callq callee
; WIN64-LABEL: call:
; WIN64:       movq $5, 32(%rsp)
; WIN64:       movl $1, %ecx
; WIN64:       callq callee
  call void @callee(i64 1, i64 2, i64 3, i64 4, i64 5, i64 6, i64 7)
  ret void
}

define i64 @csr(i64 %x) {
; CHECK-LABEL: csr:
; CHECK:       pushq %r14
; CHECK-NEXT:  .seh_pushreg %r14
; CHECK-NEXT:  pushq %r12
; CHECK-NEXT:  .seh_pushreg %r12
; CHECK-NEXT:  pushq %rbx
; CHECK-NEXT:  .seh_pushreg %rbx
; CHECK-NEXT:  .seh_endprologue
; CHECK-NEXT:  movq %rdi, %r14
; WIN64-LABEL: csr:
; WIN64:       .seh_pushreg %rsi
; WIN64:       .seh_pushreg %rdi
; WIN64:       .seh_savexmm %xmm6
  call void asm sideeffect "", "~{rbx},~{rsi},~{rdi},~{r12},~{xmm6}"()
  call void @clobber()
  ret i64 %x
}

define i128 @ret128(i128 %x) {
; CHECK-LABEL: ret128:
; CHECK:       movq %rsi, %rdx
; CHECK-NEXT:  movq %rdi, %rax
; CHECK-NEXT:  retq
  ret i128 %x
}

define double @fp(double %a, i64 %b, double %c) {
; CHECK-LABEL: fp:
; CHECK:       addsd %xmm1, %xmm0
; CHECK-NEXT:  retq
; WIN64-LABEL: fp:
; WIN64:       addsd %xmm2, %xmm0
  %r = fadd double %a, %c
  ret double %r
}

define void @va(i32 %n, ...) {
; CHECK-LABEL: va:
; CHECK:       testb %al, %al
; CHECK:       movaps %xmm7,
; CHECK:       movq %rsi,
; CHECK:       movq %r9,
; CHECK:       movabsq $206158430216, %rax
; CHECK:       movq %rsp, %rdi
; CHECK-NEXT:  callq use
; WIN64-LABEL: va:
; WIN64-NOT:   testb %al, %al
; WIN64:       movq %r9,
; WIN64:       callq use
  %ap = alloca [1 x { i32, i32, ptr, ptr }], align 16
  call void @llvm.va_start.p0(ptr %ap)
  call void @use(ptr %ap)
  call void @llvm.va_end.p0(ptr %ap)
  ret void
}

; The i128 runtime routines follow the System V convention too: operands in
; register pairs rather than by address, and the result in RDX:RAX.
define i128 @div(i128 %a, i128 %b) {
; CHECK-LABEL: div:
; CHECK-NOT:   lea
; CHECK:       callq __divti3
; CHECK-NOT:   xmm0
; CHECK:       retq
; WIN64-LABEL: div:
; WIN64:       leaq 48(%rsp), %rcx
; WIN64:       callq __divti3
; WIN64:       movq %xmm0, %rax
  %r = sdiv i128 %a, %b
  ret i128 %r
}

define double @tofp(i128 %a) {
; CHECK-LABEL: tofp:
; CHECK-NOT:   lea
; CHECK:       callq __floattidf
; WIN64-LABEL: tofp:
; WIN64:       leaq 32(%rsp), %rcx
; WIN64:       callq __floattidf
  %r = sitofp i128 %a to double
  ret double %r
}

; RUN: llc -mtriple=x86_64-pc-windows-ntposix < %s | FileCheck %s

; NT-POSIX probes large and dynamic allocations with __chkstk under the x86-64
; contract: the helper touches the pages and leaves RSP alone, and the caller
; subtracts RAX from RSP afterwards.

declare void @use(ptr)

define void @big() {
; CHECK-LABEL: big:
; CHECK:       movl $8200, %eax
; CHECK-NEXT:  callq __chkstk
; CHECK-NEXT:  subq %rax, %rsp
; CHECK-NEXT:  .seh_stackalloc 8200
; CHECK-NEXT:  .seh_endprologue
  %buf = alloca [8192 x i8], align 16
  call void @use(ptr %buf)
  ret void
}

define void @dyn(i64 %n) {
; CHECK-LABEL: dyn:
; CHECK:       .seh_endprologue
; CHECK-NEXT:  leaq 15(%rdi), %rax
; CHECK-NEXT:  andq $-16, %rax
; CHECK-NEXT:  callq __chkstk
; CHECK-NEXT:  subq %rax, %rsp
; CHECK-NEXT:  movq %rsp, %rdi
; CHECK-NEXT:  callq use
  %buf = alloca i8, i64 %n, align 16
  call void @use(ptr %buf)
  ret void
}

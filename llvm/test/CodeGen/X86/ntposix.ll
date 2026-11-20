; RUN: llc -mtriple=x86_64-pc-windows-ntposix < %s | FileCheck %s

; NT-POSIX's default convention is System V, while the Windows x64 platform's
; rules stay: unwind data, __chkstk, a trap after a trailing call and
; constructors in .CRT$XCU.

; CHECK-LABEL: args:
; CHECK:       leaq (%rdi,%rsi), %rax
define i64 @args(i64 %a, i64 %b) {
  %s = add i64 %a, %b
  ret i64 %s
}

; CHECK-LABEL: clobber:
; CHECK-NOT:   %rsi
; CHECK-NOT:   %xmm6
; CHECK:       retq
define void @clobber() nounwind {
  call void asm sideeffect "", "~{rsi},~{rdi},~{xmm6}"()
  ret void
}

; CHECK-LABEL: variadic:
; CHECK:       movb $1, %al
; CHECK:       callq v
declare i32 @v(i32, ...)
define i32 @variadic() nounwind {
  %r = call i32 (i32, ...) @v(i32 1, double 2.0)
  ret i32 %r
}

; CHECK-LABEL: probe:
; CHECK:       .seh_proc probe
; CHECK:       callq __chkstk
; CHECK-NEXT:  subq %rax, %rsp
declare void @use(ptr)
define void @probe() uwtable {
  %a = alloca [8192 x i8]
  call void @use(ptr %a)
  ret void
}

; CHECK-LABEL: trailing:
; CHECK:       callq g
; CHECK-NEXT:  int3
; CHECK-NEXT:  .Lfunc_end{{[0-9]+}}:
; CHECK-NEXT:  .size trailing,
; CHECK-NEXT:  .seh_endproc
declare void @g()
define void @trailing() uwtable {
  call void @g()
  unreachable
}

; CHECK: .section .CRT$XCU,"dr"
; CHECK: .quad g
@llvm.global_ctors = appending global [1 x { i32, ptr, ptr }] [{ i32, ptr, ptr } { i32 65535, ptr @g, ptr null }]

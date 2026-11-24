; RUN: llc -mtriple=x86_64-windows-ntposix < %s | FileCheck %s --check-prefixes=CHECK,SYSV
; RUN: llc -mtriple=x86_64-windows-itanium < %s | FileCheck %s --check-prefixes=CHECK,WIN64
; RUN: llc -O0 -mtriple=x86_64-windows-ntposix < %s | FileCheck %s --check-prefix=O0

; A function that carries "thread-pointer-argument" is passed its thread
; pointer in R14 under the C table of either environment, and its symbol
; carries $tp.

declare ptr @llvm.thread.pointer.p0()
declare void @g(i64) "thread-pointer-argument"
declare i64 @plain(i64)

; The thread pointer arrives in its register, and a call passes it in place;
; at -O0 the fast allocator keeps it across each call as any value.
; O0-LABEL:    pass$tp:
; O0:          movq %r14, (%rsp)
; O0:          callq g$tp
; O0-NEXT:     movq (%rsp), %r14
; CHECK-LABEL: pass$tp:
; CHECK-NOT:   %r14
; CHECK:       callq g$tp
; CHECK-NOT:   %r14
; CHECK:       callq g$tp
; CHECK-NOT:   %r14
; CHECK:       retq
define void @pass(i64 %a) "thread-pointer-argument" {
  %tp = call ptr @llvm.thread.pointer.p0()
  call void @g(i64 %a) [ "threadpointer"(ptr %tp) ]
  call void @g(i64 %a) [ "threadpointer"(ptr %tp) ]
  ret void
}

; A load through it addresses the register.
; CHECK-LABEL: load$tp:
; CHECK:       movq 80(%r14), %rax
define i64 @load() "thread-pointer-argument" {
  %tp = call ptr @llvm.thread.pointer.p0()
  %p = getelementptr i8, ptr %tp, i64 80
  %x = load i64, ptr %p
  ret i64 %x
}

; A tail call passes the thread pointer the caller was passed.
; SYSV-LABEL: tail$tp:
; SYSV-NEXT:  # %bb.0:
; SYSV-NEXT:  jmp g$tp
define void @tail(i64 %a) "thread-pointer-argument" {
  %tp = call ptr @llvm.thread.pointer.p0()
  tail call void @g(i64 %a) [ "threadpointer"(ptr %tp) ]
  ret void
}

; In a block apart from the thread pointer's, a call passes the register's
; incoming value, so a tail call stays one.
; CHECK-LABEL: tailbranch$tp:
; CHECK-NOT:   callq
; CHECK:       j{{mp|ne}} g$tp # TAILCALL
define void @tailbranch(i64 %a, i1 %c) "thread-pointer-argument" {
entry:
  %tp = call ptr @llvm.thread.pointer.p0()
  br i1 %c, label %t, label %e
t:
  tail call void @g(i64 %a) [ "threadpointer"(ptr %tp) ]
  br label %e
e:
  ret void
}

; Any other value is set in the register before the call and the caller's
; restored after it, so the call is no tail call.
; CHECK-LABEL: other:
; CHECK:       pushq %r14
; SYSV:        movq %rsi, %r14
; WIN64:       movq %rdx, %r14
; CHECK-NEXT:  callq g$tp
; CHECK:       popq %r14
define void @other(i64 %a, ptr %t) {
  tail call void @g(i64 %a) [ "threadpointer"(ptr %t) ]
  ret void
}

; An indirect call passes it as a direct one does.
; CHECK-LABEL: indirect$tp:
; CHECK-NOT:   %r14
; CHECK:       callq *%rax
define void @indirect(ptr %f) "thread-pointer-argument" {
  %tp = call ptr @llvm.thread.pointer.p0()
  call void %f(i64 1) [ "threadpointer"(ptr %tp) ]
  ret void
}

; Where the thread pointer is dead its register holds other values.
; SYSV-LABEL: dead$tp:
; SYSV:       movq (%rdi), %r14
; SYSV:       callq plain
; SYSV-NEXT:  addq %r14, %rax
define i64 @dead(ptr %p) "thread-pointer-argument" {
  %v = load i64, ptr %p
  call void asm sideeffect "", "~{rbx},~{rbp},~{rdi},~{rsi},~{r12},~{r13},~{r15}"()
  %r = call i64 @plain(i64 1)
  %s = add i64 %r, %v
  ret i64 %s
}

; A function that pins it reads it in the register, copies nothing to pass it
; on, and keeps every other value out of the register.
; SYSV-LABEL: pinned$tp:
; SYSV-NOT:   %r14
; SYSV:       movq %rax, 8(%rsp) # 8-byte Spill
; SYSV-NOT:   %r14
; SYSV:       callq g$tp
; SYSV-NEXT:  movq 8(%rsp), %rax # 8-byte Reload
; SYSV-NEXT:  addq 88(%r14), %rax
define i64 @pinned(ptr %p) "thread-pointer-argument" "pin-thread-pointer" {
  %tp = call ptr @llvm.thread.pointer.p0()
  %v = load i64, ptr %p
  call void asm sideeffect "", "~{rbx},~{rbp},~{rdi},~{rsi},~{r12},~{r13},~{r15}"()
  call void @g(i64 1) [ "threadpointer"(ptr %tp) ]
  %q = getelementptr i8, ptr %tp, i64 88
  %y = load i64, ptr %q
  %s = add i64 %y, %v
  ret i64 %s
}

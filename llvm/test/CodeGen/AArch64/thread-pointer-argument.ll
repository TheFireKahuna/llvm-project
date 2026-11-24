; RUN: llc -mtriple=aarch64-windows-ntposix < %s | FileCheck %s
; RUN: llc -mtriple=aarch64-windows-itanium < %s | FileCheck %s
; RUN: llc -O0 -mtriple=aarch64-windows-ntposix < %s | FileCheck %s --check-prefix=O0

; A function that carries "thread-pointer-argument" is passed its thread
; pointer in X28 on either environment, and its symbol carries $tp. At -O0
; GlobalISel leaves such a function to the DAG.

declare ptr @llvm.thread.pointer.p0()
declare void @g(i64) "thread-pointer-argument"
declare i64 @plain(i64)

; The thread pointer arrives in its register, and a call passes it in place.
; CHECK-LABEL: pass$tp:
; CHECK-NOT:   x28
; CHECK:       bl g$tp
; CHECK-NOT:   x28
; CHECK:       bl g$tp
; CHECK-NOT:   x28
; CHECK:       ret
; O0-LABEL:    pass$tp:
; O0:          bl g$tp
define void @pass(i64 %a) "thread-pointer-argument" {
  %tp = call ptr @llvm.thread.pointer.p0()
  call void @g(i64 %a) [ "threadpointer"(ptr %tp) ]
  call void @g(i64 %a) [ "threadpointer"(ptr %tp) ]
  ret void
}

; A load through it addresses the register.
; CHECK-LABEL: load$tp:
; CHECK:       ldr x0, [x28, #80]
; O0-LABEL:    load$tp:
; O0:          ldr x0, [x28, #80]
define i64 @load() "thread-pointer-argument" {
  %tp = call ptr @llvm.thread.pointer.p0()
  %p = getelementptr i8, ptr %tp, i64 80
  %x = load i64, ptr %p
  ret i64 %x
}

; A tail call passes the thread pointer the caller was passed.
; CHECK-LABEL: tail$tp:
; CHECK-NEXT:  // %bb.0:
; CHECK-NEXT:  b g$tp
define void @tail(i64 %a) "thread-pointer-argument" {
  %tp = call ptr @llvm.thread.pointer.p0()
  tail call void @g(i64 %a) [ "threadpointer"(ptr %tp) ]
  ret void
}

; In a block apart from the thread pointer's, a call passes the register's
; incoming value, so a tail call stays one.
; CHECK-LABEL: tailbranch$tp:
; CHECK-NOT:   bl g$tp
; CHECK:       b g$tp
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
; CHECK:       str x28, [sp
; CHECK:       mov x28, x1
; CHECK-NEXT:  bl g$tp
; CHECK:       ldr x28, [sp
define void @other(i64 %a, ptr %t) {
  tail call void @g(i64 %a) [ "threadpointer"(ptr %t) ]
  ret void
}

; An indirect call passes it as a direct one does.
; CHECK-LABEL: indirect$tp:
; CHECK-NOT:   x28
; CHECK:       blr x8
define void @indirect(ptr %f) "thread-pointer-argument" {
  %tp = call ptr @llvm.thread.pointer.p0()
  call void %f(i64 1) [ "threadpointer"(ptr %tp) ]
  ret void
}

; Where the thread pointer is dead its register holds other values.
; CHECK-LABEL: dead$tp:
; CHECK:       ldr x28, [x0]
; CHECK:       bl plain
; CHECK-NEXT:  add x0, x0, x28
define i64 @dead(ptr %p) "thread-pointer-argument" {
  %v = load i64, ptr %p
  call void asm sideeffect "", "~{x19},~{x20},~{x21},~{x22},~{x23},~{x24},~{x25},~{x26},~{x27}"()
  %r = call i64 @plain(i64 1)
  %s = add i64 %r, %v
  ret i64 %s
}

; A function that pins it reads it in the register, copies nothing to pass it
; on, and keeps every other value out of the register.
; CHECK-LABEL: pinned$tp:
; CHECK-NOT:   x28
; CHECK:       str x8, [sp, #8] // 8-byte Spill
; CHECK-NOT:   x28
; CHECK:       bl g$tp
; CHECK-NEXT:  ldr x8, [x28, #88]
define i64 @pinned(ptr %p) "thread-pointer-argument" "pin-thread-pointer" {
  %tp = call ptr @llvm.thread.pointer.p0()
  %v = load i64, ptr %p
  call void asm sideeffect "", "~{x19},~{x20},~{x21},~{x22},~{x23},~{x24},~{x25},~{x26},~{x27}"()
  call void @g(i64 1) [ "threadpointer"(ptr %tp) ]
  %q = getelementptr i8, ptr %tp, i64 88
  %y = load i64, ptr %q
  %s = add i64 %y, %v
  ret i64 %s
}

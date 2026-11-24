; RUN: llc -mtriple=x86_64-windows-ntposix < %s | FileCheck %s
; RUN: llc -mtriple=x86_64-windows-itanium < %s | FileCheck %s

; A function's thread pointer holds r14's incoming value, which leaves the
; register unmodified: reading it needs no frame, and holding it there does
; not make the register a used callee-saved one that another value may take
; for free.

declare ptr @llvm.thread.pointer.p0()
declare void @g(ptr) "thread-pointer-argument"
declare void @grow(ptr, ptr, i64, i64) "thread-pointer-argument"
declare void @h(ptr)

; The calls' use of r14 keeps the frame in the block that calls.
; CHECK-LABEL: sw$tp:
; CHECK-NOT:   push
; CHECK:       je [[RET:.LBB0_[0-9]+]]
; CHECK:       push
; CHECK:       callq g$tp
; CHECK:       [[RET]]:
; CHECK-NEXT:  retq
define void @sw(ptr %p, i1 %c) "thread-pointer-argument" {
entry:
  %tp = call ptr @llvm.thread.pointer.p0()
  br i1 %c, label %t, label %e
t:
  call void @g(ptr %p) [ "threadpointer"(ptr %tp) ]
  call void @g(ptr %p) [ "threadpointer"(ptr %tp) ]
  br label %e
e:
  ret void
}

; The tail call's use of r14 leaves the frame to the other path.
; CHECK-LABEL: sw2$tp:
; CHECK-NOT:   push
; CHECK:       j{{ne|mp}} g$tp # TAILCALL
; CHECK:       push
; CHECK:       callq h
define void @sw2(ptr %p, i1 %c) "thread-pointer-argument" {
entry:
  %tp = call ptr @llvm.thread.pointer.p0()
  br i1 %c, label %t, label %e
t:
  tail call void @g(ptr %p) [ "threadpointer"(ptr %tp) ]
  ret void
e:
  call void @h(ptr %p)
  call void @h(ptr %p)
  ret void
}

; Once the thread pointer is dead, a value live across a call is not given
; r14 as a callee-saved register already in use.
; CHECK-LABEL: resize$tp:
; CHECK-NOT:   %r14
; CHECK:       .seh_endproc
define void @resize(ptr %this, i64 %N) "thread-pointer-argument" {
entry:
  %tp = call ptr @llvm.thread.pointer.p0()
  %sizep = getelementptr inbounds nuw i8, ptr %this, i64 8
  %0 = load i32, ptr %sizep, align 8
  %size = zext i32 %0 to i64
  %same = icmp eq i64 %N, %size
  br i1 %same, label %exit, label %if.end

if.end:
  %smaller = icmp ult i64 %N, %size
  br i1 %smaller, label %set, label %if.end5

if.end5:
  %capp = getelementptr inbounds nuw i8, ptr %this, i64 12
  %1 = load i32, ptr %capp, align 4
  %cap = zext i32 %1 to i64
  %over = icmp ugt i64 %N, %cap
  br i1 %over, label %grow, label %reserved

grow:
  %inline = getelementptr inbounds nuw i8, ptr %this, i64 16
  tail call void @grow(ptr %this, ptr %inline, i64 %N, i64 4) [ "threadpointer"(ptr %tp) ]
  %.pre = load i32, ptr %sizep, align 8
  %.pre24 = zext i32 %.pre to i64
  br label %reserved

reserved:
  %old = phi i64 [ %size, %if.end5 ], [ %.pre24, %grow ]
  %full = icmp eq i64 %N, %old
  br i1 %full, label %set, label %fill

fill:
  %2 = load ptr, ptr %this, align 8
  %at = getelementptr [4 x i8], ptr %2, i64 %old
  %3 = sub i64 %N, %old
  %4 = shl i64 %3, 2
  tail call void @llvm.memset.p0.i64(ptr align 4 %at, i8 0, i64 %4, i1 false)
  br label %set

set:
  %n = trunc i64 %N to i32
  store i32 %n, ptr %sizep, align 8
  br label %exit

exit:
  ret void
}

; RUN: opt -passes=gvn -S < %s | FileCheck %s

; A fault load dominated by one of the same memory with no write between takes
; the earlier value and stays as its block's terminator with the edge marked
; dead, for SimplifyCFG and DCE.

declare i32 @rust_eh_personality(...)
declare i64 @llvm.fault.load.i64.p0(ptr, i32)
declare void @llvm.fault.store.i64.p0(i64, ptr, i32)

; CHECK-LABEL: @dominated(
; CHECK: %a = invoke i64 @llvm.fault.load.i64.p0(ptr %p, i32 8)
; CHECK: %b = invoke i64 @llvm.fault.load.i64.p0(ptr %p, i32 8) #[[NOUNWIND:[0-9]+]]
; CHECK: %sum = add i64 %a, %a
define i64 @dominated(ptr %p, i1 %c) personality ptr @rust_eh_personality {
entry:
  %a = invoke i64 @llvm.fault.load.i64.p0(ptr %p, i32 8) to label %branch unwind label %exception
branch:
  br i1 %c, label %again, label %normal
again:
  %b = invoke i64 @llvm.fault.load.i64.p0(ptr %p, i32 8) to label %join unwind label %exception
join:
  %sum = add i64 %a, %b
  ret i64 %sum
normal:
  ret i64 %a
exception:
  %value = landingpad { ptr, i32 } cleanup
  resume { ptr, i32 } %value
}

; CHECK-LABEL: @clobbered(
; CHECK: %b = invoke i64 @llvm.fault.load.i64.p0(ptr %p, i32 8)
; CHECK-NEXT: to label %join unwind label %exception
; CHECK: %sum = add i64 %a, %b
define i64 @clobbered(ptr %p, ptr %q, i1 %c) personality ptr @rust_eh_personality {
entry:
  %a = invoke i64 @llvm.fault.load.i64.p0(ptr %p, i32 8) to label %branch unwind label %exception
branch:
  br i1 %c, label %write, label %normal
write:
  invoke void @llvm.fault.store.i64.p0(i64 0, ptr %q, i32 8) to label %again unwind label %exception
again:
  %b = invoke i64 @llvm.fault.load.i64.p0(ptr %p, i32 8) to label %join unwind label %exception
join:
  %sum = add i64 %a, %b
  ret i64 %sum
normal:
  ret i64 %a
exception:
  %value = landingpad { ptr, i32 } cleanup
  resume { ptr, i32 } %value
}

; CHECK: attributes #[[NOUNWIND]] = { nounwind }

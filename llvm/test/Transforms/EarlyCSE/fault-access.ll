; RUN: opt -passes=early-cse -S < %s | FileCheck %s
; RUN: opt -passes='early-cse<memssa>' -S < %s | FileCheck %s

; A fault load is redundant once an earlier one read the same memory with no
; write between: its uses take the earlier value, and it stays as its block's
; terminator with the edge marked dead, for SimplifyCFG and DCE.

declare i32 @rust_eh_personality(...)
declare i64 @llvm.fault.load.i64.p0(ptr, i32)
declare i64 @llvm.fault.load.volatile.i64.p0(ptr, i32)
declare void @llvm.fault.store.i64.p0(i64, ptr, i32)

; CHECK-LABEL: @twice(
; CHECK: %a = invoke i64 @llvm.fault.load.i64.p0(ptr %p, i32 8)
; CHECK: %b = invoke i64 @llvm.fault.load.i64.p0(ptr %p, i32 8) #[[NOUNWIND:[0-9]+]]
; CHECK: %sum = add i64 %a, %a
define i64 @twice(ptr %p) personality ptr @rust_eh_personality {
entry:
  %a = invoke i64 @llvm.fault.load.i64.p0(ptr %p, i32 8) to label %again unwind label %exception
again:
  %b = invoke i64 @llvm.fault.load.i64.p0(ptr %p, i32 8) to label %normal unwind label %exception
normal:
  %sum = add i64 %a, %b
  ret i64 %sum
exception:
  %value = landingpad { ptr, i32 } cleanup
  resume { ptr, i32 } %value
}

; A store between is a clobber.
; CHECK-LABEL: @clobbered(
; CHECK: %b = invoke i64 @llvm.fault.load.i64.p0(ptr %p, i32 8)
; CHECK-NEXT: to label %normal unwind label %exception
; CHECK: %sum = add i64 %a, %b
define i64 @clobbered(ptr %p, ptr %q) personality ptr @rust_eh_personality {
entry:
  %a = invoke i64 @llvm.fault.load.i64.p0(ptr %p, i32 8) to label %write unwind label %exception
write:
  invoke void @llvm.fault.store.i64.p0(i64 0, ptr %q, i32 8) to label %again unwind label %exception
again:
  %b = invoke i64 @llvm.fault.load.i64.p0(ptr %p, i32 8) to label %normal unwind label %exception
normal:
  %sum = add i64 %a, %b
  ret i64 %sum
exception:
  %value = landingpad { ptr, i32 } cleanup
  resume { ptr, i32 } %value
}

; A volatile load is never merged.
; CHECK-LABEL: @volatile(
; CHECK: %b = invoke i64 @llvm.fault.load.volatile.i64.p0(ptr %p, i32 8)
; CHECK-NEXT: to label %normal unwind label %exception
; CHECK: %sum = add i64 %a, %b
define i64 @volatile(ptr %p) personality ptr @rust_eh_personality {
entry:
  %a = invoke i64 @llvm.fault.load.volatile.i64.p0(ptr %p, i32 8) to label %again unwind label %exception
again:
  %b = invoke i64 @llvm.fault.load.volatile.i64.p0(ptr %p, i32 8) to label %normal unwind label %exception
normal:
  %sum = add i64 %a, %b
  ret i64 %sum
exception:
  %value = landingpad { ptr, i32 } cleanup
  resume { ptr, i32 } %value
}

; CHECK: attributes #[[NOUNWIND]] = { nounwind }

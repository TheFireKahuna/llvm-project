; RUN: opt -O2 -S < %s | FileCheck %s

; The fold chain end to end: an invoke of a fault access on a local is marked
; nounwind by InstCombine, made a call by SimplifyCFG, made a plain load by
; InstCombine, and promoted by SROA, leaving no memory access at all; a raw
; pointer keeps its edge through the whole pipeline.

declare i32 @rust_eh_personality(...)
declare i64 @llvm.fault.load.i64.p0(ptr, i32)
declare void @llvm.fault.store.i64.p0(i64, ptr, i32)
declare void @cleanup_effect()

; CHECK-LABEL: @local(
; CHECK-NEXT: entry:
; CHECK-NEXT: ret i64 %seed
define i64 @local(i64 %seed) personality ptr @rust_eh_personality {
entry:
  %slot = alloca i64
  invoke void @llvm.fault.store.i64.p0(i64 %seed, ptr %slot, i32 8) to label %read unwind label %exception
read:
  %v = invoke i64 @llvm.fault.load.i64.p0(ptr %slot, i32 8) to label %normal unwind label %exception
normal:
  ret i64 %v
exception:
  %value = landingpad { ptr, i32 } cleanup
  call void @cleanup_effect()
  resume { ptr, i32 } %value
}

; CHECK-LABEL: @raw(
; CHECK: invoke i64 @llvm.fault.load.i64.p0(ptr %p, i32 8)
; CHECK-NEXT: to label %normal unwind label %exception
; CHECK: landingpad
; CHECK: call void @cleanup_effect()
define i64 @raw(ptr %p) personality ptr @rust_eh_personality {
entry:
  %v = invoke i64 @llvm.fault.load.i64.p0(ptr %p, i32 8) to label %normal unwind label %exception
normal:
  ret i64 %v
exception:
  %value = landingpad { ptr, i32 } cleanup
  call void @cleanup_effect()
  resume { ptr, i32 } %value
}

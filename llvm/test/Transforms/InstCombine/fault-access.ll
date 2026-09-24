; RUN: opt -passes=instcombine -S < %s | FileCheck %s

; A fault access is the plain access once it cannot fault: a call of one that
; cannot unwind has no edge to carry, and an access of a pointer the language
; vouches for gets nounwind, for SimplifyCFG to drop an invoke's edge and for
; a call to become the plain access. A call that may unwind keeps its edge to
; the caller.

declare i32 @rust_eh_personality(...)
declare i64 @llvm.fault.load.i64.p0(ptr, i32)
declare i64 @llvm.fault.load.volatile.i64.p0(ptr, i32)
declare void @llvm.fault.store.i64.p0(i64, ptr, i32)
declare void @llvm.fault.memcpy.p0.p0.i64(ptr, ptr, i64, i1)
declare void @llvm.fault.memset.p0.i64(ptr, i8, i64, i1)

; CHECK-LABEL: @call_load(
; CHECK-NEXT: load i64, ptr %p, align 8
; CHECK-NEXT: ret i64
define i64 @call_load(ptr %p) {
  %v = call i64 @llvm.fault.load.i64.p0(ptr %p, i32 8) #0
  ret i64 %v
}

; CHECK-LABEL: @call_load_volatile(
; CHECK-NEXT: load volatile i64, ptr %p, align 4
define i64 @call_load_volatile(ptr %p) {
  %v = call i64 @llvm.fault.load.volatile.i64.p0(ptr %p, i32 4) #0
  ret i64 %v
}

; CHECK-LABEL: @call_store(
; CHECK-NEXT: store i64 %v, ptr %p, align 8
; CHECK-NEXT: ret void
define void @call_store(ptr %p, i64 %v) {
  call void @llvm.fault.store.i64.p0(i64 %v, ptr %p, i32 8) #0
  ret void
}

; CHECK-LABEL: @call_memcpy(
; CHECK-NEXT: call void @llvm.memcpy.p0.p0.i64(ptr align 16 %d, ptr align 4 %s, i64 %n, i1 false)
define void @call_memcpy(ptr %d, ptr %s, i64 %n) {
  call void @llvm.fault.memcpy.p0.p0.i64(ptr align 16 %d, ptr align 4 %s, i64 %n, i1 false) #0
  ret void
}

; CHECK-LABEL: @call_memset(
; CHECK-NEXT: call void @llvm.memset.p0.i64(ptr %d, i8 0, i64 %n, i1 true)
define void @call_memset(ptr %d, i64 %n) {
  call void @llvm.fault.memset.p0.i64(ptr %d, i8 0, i64 %n, i1 true) #0
  ret void
}

; A call that may unwind keeps its edge to the caller at a raw pointer.
; CHECK-LABEL: @call_to_caller(
; CHECK-NEXT: %v = call i64 @llvm.fault.load.i64.p0(ptr %p, i32 8)
; CHECK-NEXT: ret i64 %v
define i64 @call_to_caller(ptr %p) {
  %v = call i64 @llvm.fault.load.i64.p0(ptr %p, i32 8)
  ret i64 %v
}

; And becomes the plain access where the language vouches for the pointer.
; CHECK-LABEL: @call_to_caller_alloca(
; CHECK-NOT: llvm.fault
; CHECK: ret i64 %seed
define i64 @call_to_caller_alloca(i64 %seed) {
  %slot = alloca i64
  store i64 %seed, ptr %slot
  %v = call i64 @llvm.fault.load.i64.p0(ptr %slot, i32 8)
  ret i64 %v
}

; An alloca cannot fault: the invoke is marked nounwind and left for
; SimplifyCFG.
; CHECK-LABEL: @invoke_alloca(
; CHECK: invoke i64 @llvm.fault.load.i64.p0(ptr nonnull %slot, i32 8) #[[NOUNWIND:[0-9]+]]
define i64 @invoke_alloca(i64 %seed) personality ptr @rust_eh_personality {
entry:
  %slot = alloca i64
  store i64 %seed, ptr %slot
  %v = invoke i64 @llvm.fault.load.i64.p0(ptr %slot, i32 8) to label %normal unwind label %exception
normal:
  ret i64 %v
exception:
  %value = landingpad { ptr, i32 } cleanup
  resume { ptr, i32 } %value
}

; A dereferenceable argument covering the access cannot fault either.
; CHECK-LABEL: @invoke_dereferenceable(
; CHECK: invoke void @llvm.fault.store.i64.p0(i64 %v, ptr {{.*}}%p, i32 8) #[[NOUNWIND]]
define void @invoke_dereferenceable(ptr dereferenceable(8) %p, i64 %v) personality ptr @rust_eh_personality {
entry:
  invoke void @llvm.fault.store.i64.p0(i64 %v, ptr %p, i32 8) to label %normal unwind label %exception
normal:
  ret void
exception:
  %value = landingpad { ptr, i32 } cleanup
  resume { ptr, i32 } %value
}

; A raw pointer keeps its edge.
; CHECK-LABEL: @invoke_raw(
; CHECK: invoke i64 @llvm.fault.load.i64.p0(ptr %p, i32 8)
; CHECK-NEXT: to label %normal unwind label %exception
define i64 @invoke_raw(ptr %p) personality ptr @rust_eh_personality {
entry:
  %v = invoke i64 @llvm.fault.load.i64.p0(ptr %p, i32 8) to label %normal unwind label %exception
normal:
  ret i64 %v
exception:
  %value = landingpad { ptr, i32 } cleanup
  resume { ptr, i32 } %value
}

; A dereferenceable argument shorter than the access keeps its edge.
; CHECK-LABEL: @invoke_short(
; CHECK: invoke i64 @llvm.fault.load.i64.p0(ptr {{.*}}%p, i32 8)
; CHECK-NEXT: to label %normal unwind label %exception
define i64 @invoke_short(ptr dereferenceable(4) %p) personality ptr @rust_eh_personality {
entry:
  %v = invoke i64 @llvm.fault.load.i64.p0(ptr %p, i32 8) to label %normal unwind label %exception
normal:
  ret i64 %v
exception:
  %value = landingpad { ptr, i32 } cleanup
  resume { ptr, i32 } %value
}

; CHECK: attributes #[[NOUNWIND]] = { nounwind }

attributes #0 = { nounwind }

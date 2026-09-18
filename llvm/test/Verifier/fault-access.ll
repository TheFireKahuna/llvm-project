; RUN: opt -passes=verify -S < %s | FileCheck %s

; The fault accesses are the intrinsics an invoke may name.

declare i32 @rust_eh_personality(...)
declare i64 @llvm.fault.load.i64.p0(ptr, i32)
declare void @llvm.fault.store.i64.p0(i64, ptr, i32)
declare void @llvm.fault.memcpy.p0.p0.i64(ptr, ptr, i64, i1)
declare void @llvm.fault.memmove.p0.p0.i64(ptr, ptr, i64, i1)
declare void @llvm.fault.memset.p0.i64(ptr, i8, i64, i1)

; CHECK-LABEL: @all(
define i64 @all(ptr %p, ptr %q, i64 %n) personality ptr @rust_eh_personality {
entry:
  %v = invoke i64 @llvm.fault.load.i64.p0(ptr %p, i32 8) to label %store unwind label %exception
store:
  invoke void @llvm.fault.store.i64.p0(i64 %v, ptr %q, i32 8) to label %copy unwind label %exception
copy:
  invoke void @llvm.fault.memcpy.p0.p0.i64(ptr %q, ptr %p, i64 %n, i1 false) to label %move unwind label %exception
move:
  invoke void @llvm.fault.memmove.p0.p0.i64(ptr %q, ptr %p, i64 %n, i1 false) to label %fill unwind label %exception
fill:
  invoke void @llvm.fault.memset.p0.i64(ptr %q, i8 0, i64 %n, i1 false) to label %normal unwind label %exception
normal:
  ret i64 %v
exception:
  %value = landingpad { ptr, i32 } cleanup
  resume { ptr, i32 } %value
}

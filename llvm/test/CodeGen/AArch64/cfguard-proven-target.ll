; RUN: llc -mtriple=aarch64-unknown-windows-itanium -verify-machineinstrs < %s \
; RUN:   | FileCheck %s

;; The CFGuard pass marks the load of an indirect call's target that it proved
;; to come from a constant table of functions of the call's KCFI type, and
;; checks the call through the KCFI check thunk or the guard check function as
;; any other. After register allocation, the check before a call whose target
;; stayed in its register from the load is dropped, with the guard function's
;; pointer and the copy of the target it took.

@table = internal constant [4 x ptr] [ptr @f, ptr @g, ptr @f, ptr @g]
@mixed = internal constant [4 x ptr] [ptr @f, ptr @h, ptr @f, ptr @g]

declare !kcfi_type !3 void @f()
declare !kcfi_type !3 void @g()
declare !kcfi_type !4 void @h()

; CHECK-LABEL: proven:
; CHECK:         ldr [[T:x[0-9]+]], [x{{[0-9]+}}, x{{[0-9]+}}, lsl #3]
; CHECK-NEXT:    blr [[T]]
; CHECK-NOT:     __llvm_kcfi_
; CHECK:         ret
define void @proven(i64 %i) {
  %idx = and i64 %i, 3
  %p = getelementptr inbounds [8 x i8], ptr @table, i64 %idx
  %f = load ptr, ptr %p
  call void %f() [ "kcfi"(i32 12) ]
  ret void
}

; CHECK-LABEL: proven_tail:
; CHECK:         ldr [[T:x[0-9]+]], [x{{[0-9]+}}, x{{[0-9]+}}, lsl #3]
; CHECK-NOT:     __llvm_kcfi_
; CHECK:         br [[T]]
define void @proven_tail(i64 %i) {
  %idx = and i64 %i, 3
  %p = getelementptr inbounds [8 x i8], ptr @table, i64 %idx
  %f = load ptr, ptr %p
  tail call void %f() [ "kcfi"(i32 12) ]
  ret void
}

;; A table holding a function of another type keeps the check.
; CHECK-LABEL: mismatch:
; CHECK:         ldr x15, [x{{[0-9]+}}, x{{[0-9]+}}, lsl #3]
; CHECK-NEXT:    bl __llvm_kcfi_check_0000000c
; CHECK-NEXT:    blr x15
define void @mismatch(i64 %i) {
  %idx = and i64 %i, 3
  %p = getelementptr inbounds [8 x i8], ptr @mixed, i64 %idx
  %f = load ptr, ptr %p
  call void %f() [ "kcfi"(i32 12) ]
  ret void
}

;; A call without a KCFI type is proven by any table of functions, and loses
;; its guard check.
; CHECK-LABEL: untyped:
; CHECK-NOT:     __guard_check_icall_fptr
; CHECK:         ldr [[T:x[0-9]+]], [x{{[0-9]+}}, x{{[0-9]+}}, lsl #3]
; CHECK-NEXT:    blr [[T]]
; CHECK-NOT:     __guard_check_icall_fptr
; CHECK:         ret
define void @untyped(i64 %i) {
  %idx = and i64 %i, 3
  %p = getelementptr inbounds [8 x i8], ptr @mixed, i64 %idx
  %f = load ptr, ptr %p
  call void %f()
  ret void
}

!llvm.module.flags = !{!0, !1, !2}
!0 = !{i32 4, !"kcfi", i32 1}
!1 = !{i32 4, !"function-type-prefix", i32 -559038737}
!2 = !{i32 2, !"cfguard", i32 2}
!3 = !{i32 12}
!4 = !{i32 13}

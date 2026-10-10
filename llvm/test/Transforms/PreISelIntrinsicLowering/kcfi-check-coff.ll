; RUN: opt -mtriple=x86_64-unknown-windows-itanium -passes=pre-isel-intrinsic-lowering -S < %s | FileCheck %s
; RUN: opt -mtriple=aarch64-unknown-windows-itanium -passes=pre-isel-intrinsic-lowering -S < %s | FileCheck %s

;; On COFF, where the prefixes carry a marker, a check at either type word of
;; a prefix is left for the CFGuard pass, which routes it to a per-type thunk,
;; and a check at any other offset is expanded.

; CHECK-LABEL: define void @f(
; CHECK-NEXT:    call void @llvm.kcfi.check(ptr %p, i32 1, i32 4)
; CHECK-NEXT:    call void @llvm.kcfi.check(ptr %p, i32 2, i32 16)
; CHECK-NEXT:    [[ADDR:%.*]] = getelementptr i8, ptr %p, i64 -8
; CHECK-NEXT:    [[WORD:%.*]] = load i32, ptr [[ADDR]], align 1
; CHECK-NEXT:    [[NE:%.*]] = icmp ne i32 [[WORD]], 3
; CHECK-NEXT:    br i1 [[NE]]
define void @f(ptr %p) {
  call void @llvm.kcfi.check(ptr %p, i32 1, i32 4)
  call void @llvm.kcfi.check(ptr %p, i32 2, i32 16)
  call void @llvm.kcfi.check(ptr %p, i32 3, i32 8)
  call void %p()
  ret void
}

!llvm.module.flags = !{!0}
!0 = !{i32 4, !"function-type-prefix", i32 -559038737}

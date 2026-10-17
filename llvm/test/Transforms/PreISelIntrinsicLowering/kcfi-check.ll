; RUN: opt -mtriple=x86_64-unknown-linux-gnu -passes=pre-isel-intrinsic-lowering -S < %s | FileCheck %s
; RUN: opt -mtriple=aarch64-unknown-linux-gnu -passes=pre-isel-intrinsic-lowering -S < %s | FileCheck %s

;; Where no target lowers it, llvm.kcfi.check becomes a load of the type word
;; at the given offset before the target, a compare and a trap.

; CHECK-LABEL: define void @f(
; CHECK-NEXT:    [[ADDR:%.*]] = getelementptr i8, ptr %p, i64 -16
; CHECK-NEXT:    [[WORD:%.*]] = load i32, ptr [[ADDR]], align 1
; CHECK-NEXT:    [[NE:%.*]] = icmp ne i32 [[WORD]], 305419896
; CHECK-NEXT:    br i1 [[NE]], label %[[TRAP:.*]], label %[[CONT:.*]], !prof [[PROF:![0-9]+]]
; CHECK:       [[TRAP]]:
; CHECK-NEXT:    call void @llvm.trap()
; CHECK-NEXT:    unreachable
; CHECK:       [[CONT]]:
; CHECK-NEXT:    call void %p()
; CHECK-NEXT:    ret void
; CHECK: [[PROF]] = !{!"branch_weights", i32 1, i32 1048575}
define void @f(ptr %p) {
  call void @llvm.kcfi.check(ptr %p, i32 305419896, i32 16)
  call void %p()
  ret void
}

; RUN: opt -S -passes='pgo-icall-prom,function(cfguard)' %s | FileCheck %s
; RUN: sed 's/x86_64-unknown-windows-itanium/x86_64-pc-windows-ntposix/' %s \
; RUN:   | opt -S -passes='pgo-icall-prom,function(cfguard)' | FileCheck %s
; RUN: sed 's/x86_64-unknown-windows-itanium/aarch64-unknown-windows-itanium/' %s \
; RUN:   | opt -S -passes='pgo-icall-prom,function(cfguard)' | FileCheck %s

;; Indirect-call promotion turns the hot target of a profiled indirect call
;; into a direct call, which is neither guarded nor checked by KCFI. The
;; remaining indirect call is still checked.

target triple = "x86_64-unknown-windows-itanium"

define void @callee() {
  ret void
}

; CHECK-LABEL: define void @caller(
; CHECK:         [[CMP:%.*]] = icmp eq ptr %fp, @callee
; CHECK-NEXT:    br i1 [[CMP]], label %[[DIRECT:[^,]+]], label %[[INDIRECT:[^,]+]]
; CHECK:       [[DIRECT]]:
; CHECK-NEXT:    call void @callee()
; CHECK-NOT:     __llvm_kcfi_
; CHECK:       [[INDIRECT]]:
; CHECK-NEXT:    call {{.*}}@__llvm_kcfi_{{dispatch|check}}_0000000c(
define void @caller(ptr %fp) {
  call void %fp() [ "kcfi"(i32 12) ], !prof !3
  ret void
}

!llvm.module.flags = !{!0, !1, !2}
!0 = !{i32 4, !"kcfi", i32 1}
!1 = !{i32 4, !"function-type-prefix", i32 -559038737}
!2 = !{i32 2, !"cfguard", i32 2}
!3 = !{!"VP", i32 0, i64 1000, i64 -6621307527791283157, i64 1000}

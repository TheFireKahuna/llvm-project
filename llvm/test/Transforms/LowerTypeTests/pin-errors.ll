; RUN: not opt -S -passes=lowertypetests %s -o /dev/null 2>&1 | FileCheck %s

; A required pin that cannot be kept in the combined global is an error.

target datalayout = "e-p:64:64"

; CHECK: error: pin of 'aligned' cannot be kept in a combined global
@aligned = constant [2 x ptr] zeroinitializer, align 64, !type !0, !pin !10

; CHECK: error: pin of 'twice' cannot be kept in a combined global
@twice = constant [2 x ptr] zeroinitializer, align 8, !type !0, !pin !11, !pin !12

!0 = !{i64 0, !"typeid"}
!10 = !{i64 0, i64 12, i64 8, i64 1}
!11 = !{i64 0, i64 12, i64 64, i64 1}
!12 = !{i64 0, i64 12, i64 128, i64 1}

declare i1 @llvm.type.test(ptr, metadata)

define i1 @f(ptr %p) {
  %x = call i1 @llvm.type.test(ptr %p, metadata !"typeid")
  ret i1 %x
}

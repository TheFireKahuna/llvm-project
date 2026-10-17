; RUN: not opt -passes=verify -disable-output %s 2>&1 | FileCheck %s

; CHECK: vtable_hierarchy metadata must have three operands
@a = constant [3 x ptr] zeroinitializer, !vtable_hierarchy !0
; CHECK: vtable_hierarchy metadata's offset must be an integer
@b = constant [3 x ptr] zeroinitializer, !vtable_hierarchy !1
; CHECK: vtable_hierarchy metadata's offset must be within its global
@c = constant [3 x ptr] zeroinitializer, !vtable_hierarchy !2
; CHECK: vtable_hierarchy metadata's class must be a nonempty string
@d = constant [3 x ptr] zeroinitializer, !vtable_hierarchy !3
; CHECK: vtable_hierarchy metadata's base must be a string
@e = constant [3 x ptr] zeroinitializer, !vtable_hierarchy !4
; CHECK: vtable_hierarchy metadata must be on a global variable definition
@f = external constant [3 x ptr], !vtable_hierarchy !5
; CHECK-NOT: vtable_hierarchy metadata
@g = constant [3 x ptr] zeroinitializer, !vtable_hierarchy !5
@h = constant [3 x ptr] zeroinitializer, !vtable_hierarchy !6

!0 = !{i64 16, !"1A"}
!1 = !{!"16", !"1A", !""}
!2 = !{i64 32, !"1A", !""}
!3 = !{i64 16, !"", !""}
!4 = !{i64 16, !"1A", i64 0}
!5 = !{i64 16, !"1A", !""}
!6 = !{i64 24, !"1B", !"1A"}

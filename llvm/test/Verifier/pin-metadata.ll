; RUN: not opt -passes=verify -disable-output %s 2>&1 | FileCheck %s

; CHECK: pin metadata must have four operands
@a = constant i64 0, !pin !0
; CHECK: pin metadata operands must be integers
@b = constant i64 0, !pin !1
; CHECK: pin metadata's modulus must be at most 2^63
@c = constant i64 0, !pin !2
; CHECK: pin metadata's residue must be less than its modulus
@e = constant i64 0, !pin !4
; CHECK: pin metadata's required flag must be 0 or 1
@f = constant i64 0, !pin !5
; CHECK: pin metadata must be on a global variable definition
@g = external constant i64, !pin !3
; CHECK: pin metadata's offset must be within its global
@h = constant i64 0, !pin !6
; CHECK-NOT: pin metadata
@i = constant i64 0, !pin !7
@d = constant i64 0, !pin !3

!0 = !{i64 0, i64 12, i64 8}
!1 = !{i64 0, i64 12, !"x", i64 1}
!2 = !{i64 0, i64 64, i64 8, i64 1}
!3 = !{i64 0, i64 12, i64 8, i64 1}
!4 = !{i64 0, i64 6, i64 64, i64 1}
!5 = !{i64 0, i64 6, i64 8, i64 2}
!6 = !{i64 9, i64 6, i64 8, i64 0}
!7 = !{i64 8, i64 6, i64 8, i64 0}

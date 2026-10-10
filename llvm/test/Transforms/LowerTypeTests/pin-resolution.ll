; RUN: rm -rf %t && split-file %s %t
; RUN: opt -passes=lowertypetests -lowertypetests-summary-action=export \
; RUN:   -lowertypetests-read-summary=%t/summary.yaml \
; RUN:   -lowertypetests-write-summary=- %t/main.ll -o /dev/null | FileCheck %s

; Pinned members keep the resolution a type identifier would have without
; pins: A's members, each on a line of its own, are tested by a range alone,
; as without pins. A wide pin fixes where B's tagged member lies, so B's test
; needs an inline bit set, though still no byte array.

; CHECK:      TypeIdMap:
; CHECK:        TTRes:
; CHECK-NEXT:     Kind: AllOnes
; CHECK-NEXT:     SizeM1BitWidth: 7
; CHECK-NEXT:     AlignLog2: 6
; CHECK-NEXT:     SizeM1: 2
; CHECK:        TTRes:
; CHECK-NEXT:     Kind: Inline

;--- main.ll
target datalayout = "e-p:64:64"

@a1 = constant [3 x ptr] zeroinitializer, align 8, !type !0, !pin !10
@a2 = constant [3 x ptr] zeroinitializer, align 8, !type !0, !pin !10
@a3 = constant [3 x ptr] zeroinitializer, align 8, !type !0, !pin !10
@b1 = constant [3 x ptr] zeroinitializer, align 8, !type !1, !pin !10
@b2 = constant [3 x ptr] zeroinitializer, align 8, !type !1, !pin !10
@b3 = constant [3 x ptr] zeroinitializer, align 8, !type !1, !pin !11

!0 = !{i64 16, !"A"}
!1 = !{i64 16, !"B"}
!10 = !{i64 16, i64 6, i64 16, i64 0}
!11 = !{i64 16, i64 12, i64 424, i64 1}

;--- summary.yaml
---
GlobalValueMap:
  42:
    - Live: true
      TypeTests: [ 12110082535487358335, 14608648041743670941 ]
...

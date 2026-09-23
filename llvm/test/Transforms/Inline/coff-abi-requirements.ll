; RUN: opt -passes='coff-abi-requirements,inline,instcombine,globaldce' -S %s | FileCheck %s
; RUN: opt -passes='coff-abi-requirements,instcombine,mergefunc' -S %s | FileCheck %s --check-prefix=MERGE
; RUN: opt -passes='coff-abi-requirements,coff-abi-requirements' -S %s | FileCheck %s --check-prefix=CYCLE

; Assumptions follow the surviving function even when an inline call or folded
; load leaves no instruction referencing the descriptor. Recursive call graphs
; terminate; aliases do not detach the use's original contract.
target triple = "x86_64-pc-windows-itanium"
@a = constant i32 1, !coff.abi !0
@b = constant i32 1, !coff.abi !1
@alias = alias i32, ptr @a

define internal i32 @inner() {
  %v = load i32, ptr @alias
  ret i32 %v
}
define i32 @caller() {
  %v = call i32 @inner()
  ret i32 %v
}
define i32 @equivalent() {
  %v = load i32, ptr @b
  ret i32 %v
}
define internal void @cycle1() {
  %v = load volatile i32, ptr @b
  call void @cycle2()
  ret void
}
define void @cycle2() {
  %v = load volatile i32, ptr @a
  call void @cycle1()
  ret void
}

!0 = !{!"contract-a"}
!1 = !{!"contract-b"}

; CHECK: define i32 @caller() !coff.abi.uses ![[USES:[0-9]+]]
; CHECK-NEXT: ret i32 1
; CHECK: ![[USES]] = !{![[USE:[0-9]+]]}
; CHECK: ![[USE]] = !{ptr @a, !"contract-a"}
; MERGE: !{ptr @a, !"contract-a"}
; MERGE: !{ptr @b, !"contract-b"}

; Each recursive consumer acquires both original contracts exactly once, even
; when the worklist converges through the cycle and the pass runs again.
; CYCLE: @llvm.coff.abi.keep = private constant [2 x ptr]
; CYCLE: define internal void @cycle1() !coff.abi.uses ![[BOTH:[0-9]+]]
; CYCLE: define void @cycle2() !coff.abi.uses ![[BOTH]]
; CYCLE-DAG: ![[BOTH]] = !{![[B:[0-9]+]], ![[A:[0-9]+]]}
; CYCLE-DAG: ![[B]] = !{ptr @b, !"contract-b"}
; CYCLE-DAG: ![[A]] = !{ptr @a, !"contract-a"}

; RUN: llc -mtriple=x86_64-w64-windows-gnu < %s | FileCheck %s
; RUN: %if aarch64-registered-target %{ llc -mtriple=aarch64-w64-windows-gnu < %s | FileCheck %s %}

; Under Windows EH the unwinder enters a landing pad by restoring a context
; whose instruction pointer is the pad, so with ehcontguard the pad is listed
; as an EH continuation target, once.

; CHECK-LABEL: f:
; CHECK:       [[PAD:\$ehgcr_[0-9_]+]]:
; CHECK:       .section .gehcont$y,"dr"
; CHECK-NEXT:  .symidx [[PAD]]
; CHECK-NOT:   .symidx
define void @f() personality ptr @__gxx_personality_seh0 {
  invoke void @g() to label %done unwind label %lpad
lpad:
  %lp = landingpad { ptr, i32 } cleanup
  call void @g()
  resume { ptr, i32 } %lp
done:
  ret void
}

declare void @g()
declare i32 @__gxx_personality_seh0(...)

!llvm.module.flags = !{!0}
!0 = !{i32 1, !"ehcontguard", i32 1}

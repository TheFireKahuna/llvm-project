; RUN: llc -mtriple=aarch64-unknown-windows-itanium -O0 < %s | FileCheck %s
; RUN: llc -mtriple=aarch64-unknown-windows-itanium -O2 < %s | FileCheck %s
; RUN: llc -mtriple=aarch64-w64-windows-gnu -O2 < %s | FileCheck %s

; Under Windows EH the unwinder enters a landing pad by restoring a context,
; so with ehcontguard the pad is listed as an EH continuation target, once.

define void @f() personality ptr @__gxx_personality_seh0 {
entry:
  invoke void @g()
          to label %done unwind label %lpad

lpad:
  %lp = landingpad { ptr, i32 }
          cleanup
  call void @h()
  resume { ptr, i32 } %lp

done:
  ret void
}

; CHECK-LABEL: f:
; CHECK:       [[PAD:\$ehgcr_[0-9_]+]]:
; CHECK:       .section .gehcont$y,"dr"
; CHECK-NEXT:  .symidx [[PAD]]
; CHECK-NOT:   .symidx

declare void @g()
declare void @h()
declare i32 @__gxx_personality_seh0(...)

!llvm.module.flags = !{!0}
!0 = !{i32 1, !"ehcontguard", i32 1}

; RUN: llc < %s -mtriple=x86_64-unknown-windows-itanium | FileCheck %s
; RUN: llc < %s -mtriple=x86_64-pc-windows-gnu | FileCheck %s
; RUN: llc < %s -mtriple=x86_64-unknown-windows-itanium -O0 | FileCheck %s

; Under Windows EH the unwinder enters an Itanium landing pad by setting the
; instruction pointer, so EHCont Guard lists the landing pad as a target.

; CHECK: @feat.00 = 16384
; CHECK-LABEL: f:
; CHECK: $ehgcr_0_{{[0-9]+}}:
; CHECK: .section .gehcont$y
; CHECK-NEXT: .symidx $ehgcr_0_{{[0-9]+}}

define void @f() personality ptr @__gxx_personality_seh0 {
entry:
  invoke void @g()
          to label %cont unwind label %lpad
lpad:
  %lp = landingpad { ptr, i32 }
          catch ptr null
  call void @h()
  br label %cont
cont:
  ret void
}

declare void @g()
declare void @h()
declare i32 @__gxx_personality_seh0(...)

!llvm.module.flags = !{!0}
!0 = !{i32 1, !"ehcontguard", i32 1}

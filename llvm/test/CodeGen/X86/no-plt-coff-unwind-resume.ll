; RUN: llc < %s -mtriple=x86_64-unknown-windows-itanium | FileCheck %s

; The rewind function is created by exception lowering, not by the front end,
; so nothing marks it. Under -fno-plt it takes the import form like any other
; call to a function this module does not define.

define void @f() personality ptr @__gxx_personality_seh0 {
; CHECK-LABEL: f:
; CHECK: callq *__imp__Unwind_Resume(%rip)
entry:
  invoke void @g() to label %done unwind label %lpad

lpad:
  %v = landingpad { ptr, i32 } cleanup
  call void @cleanup()
  resume { ptr, i32 } %v

done:
  ret void
}

declare void @g()
declare void @cleanup()
declare i32 @__gxx_personality_seh0(...)

!llvm.module.flags = !{!0}
!0 = !{i32 7, !"RtLibUseGOT", i32 1}

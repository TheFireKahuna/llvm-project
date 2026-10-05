; RUN: llc < %s -mtriple=x86_64-pc-windows-msvc | FileCheck %s
; RUN: llc < %s -mtriple=aarch64-pc-windows-msvc | FileCheck %s
; A personality routine is named by the unwind data of the functions that use
; it, and the system calls it directly; that use does not make it a valid
; indirect-call target. A routine whose address is taken stays listed.

; CHECK:     .section .gfids$y
; CHECK-NOT: .symidx __gxx_personality_seh0
; CHECK:     .symidx taken
; CHECK-NOT: .symidx __gxx_personality_seh0

declare i32 @__gxx_personality_seh0(...)
declare void @may_throw()
declare void @taken()

@ptr = global ptr @taken

define void @f() personality ptr @__gxx_personality_seh0 {
entry:
  invoke void @may_throw()
          to label %done unwind label %lpad
lpad:
  %0 = landingpad { ptr, i32 }
          cleanup
  resume { ptr, i32 } %0
done:
  ret void
}

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"cfguard", i32 2}

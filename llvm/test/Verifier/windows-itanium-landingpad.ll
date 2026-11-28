; RUN: not opt -passes=verify -disable-output < %s 2>&1 | FileCheck %s

; Windows Itanium's unwinder lands only in catchpads.

target triple = "x86_64-unknown-windows-itanium"

; CHECK: LandingPadInst not allowed under a Windows Itanium personality
define void @f() personality ptr @__gxx_personality_seh0 {
entry:
  invoke void @g() to label %done unwind label %lpad
lpad:
  %lp = landingpad { ptr, i32 } cleanup
  resume { ptr, i32 } %lp
done:
  ret void
}

declare void @g()
declare i32 @__gxx_personality_seh0(...)

; RUN: llc < %s -mtriple=x86_64-pc-windows-msvc | FileCheck %s
; RUN: llc < %s -mtriple=aarch64-pc-windows-msvc | FileCheck %s
; A personality routine is named by the unwind data of the functions that use
; it, and the system calls it directly; that use does not make it a valid
; indirect-call target. A routine whose address is taken stays listed. MSVC
; lists neither __C_specific_handler nor __CxxFrameHandler3 in the objects it
; builds with /guard:cf.

; CHECK:     .section .gfids$y
; CHECK-NOT: .symidx {{__gxx_personality_seh0|__C_specific_handler|__CxxFrameHandler3}}
; CHECK:     .symidx taken
; CHECK-NOT: .symidx {{__gxx_personality_seh0|__C_specific_handler|__CxxFrameHandler3}}

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

declare i32 @__C_specific_handler(...)
declare i32 @__CxxFrameHandler3(...)

define void @seh() personality ptr @__C_specific_handler {
entry:
  invoke void @may_throw()
          to label %done unwind label %pad
pad:
  %cs = catchswitch within none [label %handler] unwind to caller
handler:
  %cp = catchpad within %cs [ptr null]
  catchret from %cp to label %done
done:
  ret void
}

define void @cxx() personality ptr @__CxxFrameHandler3 {
entry:
  invoke void @may_throw()
          to label %done unwind label %pad
pad:
  %cs = catchswitch within none [label %handler] unwind to caller
handler:
  %cp = catchpad within %cs [ptr null, i32 64, ptr null]
  catchret from %cp to label %done
done:
  ret void
}

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"cfguard", i32 2}

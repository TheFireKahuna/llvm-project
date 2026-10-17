; RUN: llc -mtriple=aarch64-pc-windows-msvc %s -o - | FileCheck %s

; Import call optimization calls an imported function through a register,
; which names no function, but the function it calls is known: one that does
; not unwind leaves the state between the invokes around it unchanged.

define void @f() personality ptr @__CxxFrameHandler3 {
entry:
  invoke void @may_throw()
          to label %cont unwind label %dispatch
cont:
  call void @nothrow()
  invoke void @may_throw()
          to label %exit unwind label %dispatch
dispatch:
  %cs = catchswitch within none [label %catch] unwind to caller
catch:
  %cp = catchpad within %cs [ptr null, i32 64, ptr null]
  catchret from %cp to label %exit
exit:
  ret void
}

; CHECK-LABEL: f:
; CHECK:         blr
; CHECK-LABEL: $ip2state$f:
; CHECK-NEXT:    .word .Lfunc_begin0@IMGREL
; CHECK-NEXT:    .word -1
; CHECK-NEXT:    .word [[BEGIN:\.Ltmp[0-9]+]]@IMGREL
; CHECK-NEXT:    .word 0
; CHECK-NEXT:    .word [[END:\.Ltmp[0-9]+]]@IMGREL
; CHECK-NEXT:    .word -1
; CHECK-NEXT:    .word "?catch$3@?0?f@4HA"@IMGREL

declare void @may_throw()
declare dllimport void @nothrow() nounwind
declare i32 @__CxxFrameHandler3(...)

!llvm.module.flags = !{!0}
!0 = !{i32 1, !"import-call-optimization", i32 1}

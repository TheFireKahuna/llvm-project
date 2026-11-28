; RUN: opt -mtriple=x86_64-unknown-windows-itanium -passes=win-eh-prepare -S < %s | FileCheck %s
; RUN: opt -mtriple=aarch64-unknown-windows-itanium -passes=win-eh-prepare -S < %s | FileCheck %s

; What WinEHPrepare leaves of a function whose cleanups are funclets that the
; unwinder calls, beyond what llc's tests of them show.

declare void @may_throw()
declare i32 @get()
declare i32 @__gxx_personality_seh0(...)

; A funclet returns to the unwinder, so a value it hands the handler behind it
; goes through the frame.
define i32 @value() personality ptr @__gxx_personality_seh0 {
; CHECK-LABEL: define i32 @value(
; CHECK:       cleanup:
; CHECK:         store i32 %v, ptr [[SLOT:%.*]], align 4
; CHECK-NEXT:    cleanupret from %pad unwind label %cs
; CHECK:       handler:
; CHECK-NEXT:    [[V:%.*]] = load i32, ptr [[SLOT]], align 4
; CHECK-NEXT:    ret i32 [[V]]
entry:
  invoke void @may_throw() to label %cont unwind label %cleanup
cont:
  ret i32 0
cleanup:
  %pad = cleanuppad within none []
  %v = call i32 @get() [ "funclet"(token %pad) ]
  cleanupret from %pad unwind label %cs
cs:
  %s = catchswitch within none [label %catch] unwind to caller
catch:
  %cp = catchpad within %s [ptr null]
  catchret from %cp to label %handler
handler:
  ret i32 %v
}

; Nothing unwinds out of a funclet that the unwinder calls: an invoke in one
; is a call, and a pad nested in one goes.
define void @no_unwind() personality ptr @__gxx_personality_seh0 {
; CHECK-LABEL: define void @no_unwind(
; CHECK:       cleanup:
; CHECK-NEXT:    %pad = cleanuppad within none []
; CHECK-NEXT:    call void @may_throw() [ "funclet"(token %pad) ]
; CHECK-NEXT:    br label %next
; CHECK-NOT:   catch
entry:
  invoke void @may_throw() to label %cont unwind label %cleanup
cont:
  ret void
cleanup:
  %pad = cleanuppad within none []
  invoke void @may_throw() [ "funclet"(token %pad) ]
          to label %next unwind label %cs
next:
  cleanupret from %pad unwind to caller
cs:
  %s = catchswitch within %pad [label %catch] unwind to caller
catch:
  %cp = catchpad within %s [ptr null]
  catchret from %cp to label %next
}

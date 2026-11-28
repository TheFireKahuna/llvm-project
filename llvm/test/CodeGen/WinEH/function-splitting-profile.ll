; RUN: llc -mtriple=x86_64-unknown-windows-itanium -verify-machineinstrs < %s \
; RUN:   | FileCheck %s --implicit-check-not=hot_handler.cold
; RUN: %if aarch64-registered-target %{ llc -mtriple=aarch64-unknown-windows-itanium -verify-machineinstrs < %s \
; RUN:   | FileCheck %s --implicit-check-not=hot_handler.cold %}

; A profile decides for a function's handlers: they move where it shows all of
; them cold and stay where it shows one hot. Cleanups move whatever it shows,
; and a function with no profile takes the static rule, under which a handler
; is cold.

declare void @may_throw()
declare void @g(i32)
declare void @dtor(ptr) nounwind
declare i32 @__gxx_personality_seh0(...)

; CHECK-LABEL: cold_handler:
; CHECK:       cold_handler.cold:
; CHECK:       {{#|//}} %catch
define i32 @cold_handler(i32 %x) uwtable personality ptr @__gxx_personality_seh0 !prof !14 {
entry:
  invoke void @may_throw() to label %ok unwind label %cs, !prof !15
cs:
  %s = catchswitch within none [label %catch] unwind to caller
catch:
  %p = catchpad within %s [ptr null]
  catchret from %p to label %handled
handled:
  call void @g(i32 2)
  br label %ok
ok:
  ret i32 %x
}

; CHECK-LABEL: hot_handler:
; CHECK:       {{#|//}} %catch
; CHECK:       .Lfunc_end{{[0-9]+}}:
define i32 @hot_handler(i32 %x) uwtable personality ptr @__gxx_personality_seh0 !prof !14 {
entry:
  invoke void @may_throw() to label %ok unwind label %cs, !prof !16
cs:
  %s = catchswitch within none [label %catch] unwind to caller
catch:
  %p = catchpad within %s [ptr null]
  catchret from %p to label %handled
handled:
  call void @g(i32 2)
  br label %ok
ok:
  ret i32 %x
}

; The cleanup moves though the profile shows the unwind taken, and the hot
; handler stays.
; CHECK-LABEL: hot_handler_cleanup:
; CHECK:       {{#|//}} %catch
; CHECK:       .section .text$split,"xr"
; CHECK:       hot_handler_cleanup.cold: {{.*}} %cleanup
define i32 @hot_handler_cleanup(i32 %x) uwtable personality ptr @__gxx_personality_seh0 !prof !14 {
entry:
  %a = alloca i32
  invoke void @may_throw() to label %ok unwind label %cleanup, !prof !16
cleanup:
  %cp = cleanuppad within none []
  call void @dtor(ptr %a) [ "funclet"(token %cp) ]
  cleanupret from %cp unwind label %cs
cs:
  %s = catchswitch within none [label %catch] unwind to caller
catch:
  %p = catchpad within %s [ptr null]
  catchret from %p to label %handled
handled:
  call void @g(i32 2)
  br label %ok
ok:
  call void @dtor(ptr %a)
  ret i32 %x
}

; CHECK-LABEL: no_profile:
; CHECK:       no_profile.cold:
; CHECK:       {{#|//}} %catch
define i32 @no_profile(i32 %x) uwtable personality ptr @__gxx_personality_seh0 {
entry:
  invoke void @may_throw() to label %ok unwind label %cs
cs:
  %s = catchswitch within none [label %catch] unwind to caller
catch:
  %p = catchpad within %s [ptr null]
  catchret from %p to label %handled
handled:
  call void @g(i32 2)
  br label %ok
ok:
  ret i32 %x
}

!llvm.module.flags = !{!0}
!0 = !{i32 1, !"ProfileSummary", !1}
!1 = !{!2, !3, !4, !5, !6, !7, !8, !9}
!2 = !{!"ProfileFormat", !"InstrProf"}
!3 = !{!"TotalCount", i64 10000}
!4 = !{!"MaxCount", i64 10}
!5 = !{!"MaxInternalCount", i64 1}
!6 = !{!"MaxFunctionCount", i64 1000}
!7 = !{!"NumCounts", i64 3}
!8 = !{!"NumFunctions", i64 5}
!9 = !{!"DetailedSummary", !10}
!10 = !{!11, !12, !13}
!11 = !{i32 10000, i64 100, i32 1}
!12 = !{i32 999900, i64 100, i32 1}
!13 = !{i32 999999, i64 1, i32 2}
!14 = !{!"function_entry_count", i64 7000}
!15 = !{!"branch_weights", i32 7000, i32 0}
!16 = !{!"branch_weights", i32 3000, i32 4000}

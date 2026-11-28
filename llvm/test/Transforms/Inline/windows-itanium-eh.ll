; RUN: opt -passes=inline -S < %s | FileCheck %s
; RUN: opt -passes='print<inline-cost>' -disable-output < %s 2>&1 \
; RUN:   | FileCheck %s --check-prefix=COST

; A cleanup that Windows Itanium's unwinder calls does not unwind: a call in
; one is nested in the funclet, and a callee with a pad of its own stays a
; call, whose frame keeps its handlers. A terminate scope is no such pad: an
; unwind out of the cleanup ends in its own table, which terminates, so the
; scope goes and the calls it covered unwind out of the cleanup. A handler is
; code of its function, and takes a callee's pads. The cost of inlining into a
; cleanup counts no terminate scope.
;
; Where exception tables cover every instruction, every call in an EH scope is
; an invoke that unwinds to the scope's pad, whatever the callee throws: an
; asynchronous exception may come out of it, and the scopes of a callee that
; is inlined at it lie inside the scope.

target triple = "x86_64-unknown-windows-itanium"

declare void @may_throw()
declare void @quiet() nounwind
declare void @dtor(ptr) nounwind
declare i32 @__gxx_personality_seha0(...)
declare i32 @__gxx_personality_seh0(...)

define internal void @leaf() personality ptr @__gxx_personality_seh0 {
  call void @may_throw()
  ret void
}

define internal void @catches() personality ptr @__gxx_personality_seh0 {
entry:
  invoke void @may_throw() to label %ret unwind label %cs
ret:
  ret void
cs:
  %s = catchswitch within none [label %catch] unwind to caller
catch:
  %cp = catchpad within %s [ptr null]
  catchret from %cp to label %ret
}

declare ptr @llvm.eh.exceptionpointer.p0(token)
declare void @__clang_call_terminate(ptr)

; COST-LABEL: Analyzing call of terminates... (caller:in_funclet)
; COST:       terminate:
; COST-NEXT:  ; No analysis for the instruction
; COST-NEXT:    %s = catchswitch
; COST:         Cost: 0
define linkonce_odr void @terminates() personality ptr @__gxx_personality_seh0 {
entry:
  invoke void @may_throw() to label %ret unwind label %terminate
ret:
  ret void
terminate:
  %s = catchswitch within none [label %handler] unwind to caller
handler:
  %cp = catchpad within %s [ptr null]
  %e = call ptr @llvm.eh.exceptionpointer.p0(token %cp)
  catchret from %cp to label %body
body:
  call void @__clang_call_terminate(ptr %e)
  unreachable
}

; CHECK-LABEL: define void @in_funclet(
; CHECK:         %pad = cleanuppad within none []
; CHECK-NEXT:    call void @may_throw() [ "funclet"(token %pad) ]
; CHECK-NEXT:    call void @catches() [ "funclet"(token %pad) ]
; CHECK-NEXT:    call void @may_throw() [ "funclet"(token %pad) ]
; CHECK-NOT:     catchswitch
; CHECK:         cleanupret from %pad unwind to caller
define void @in_funclet() personality ptr @__gxx_personality_seh0 {
entry:
  invoke void @may_throw() to label %ret unwind label %cleanup
ret:
  ret void
cleanup:
  %pad = cleanuppad within none []
  call void @leaf() [ "funclet"(token %pad) ]
  call void @catches() [ "funclet"(token %pad) ]
  call void @terminates() [ "funclet"(token %pad) ]
  cleanupret from %pad unwind to caller
}

; CHECK-LABEL: define void @in_handler(
; CHECK:         %cp = catchpad within %s [ptr null]
; CHECK-NEXT:    invoke void @may_throw() [ "funclet"(token %cp) ]
; CHECK-NEXT:            to label %{{.*}} unwind label %[[CS:.*]]
; CHECK:       [[CS]]:
; CHECK-NEXT:    catchswitch within %cp
define void @in_handler() personality ptr @__gxx_personality_seh0 {
entry:
  invoke void @may_throw() to label %ret unwind label %cs
ret:
  ret void
cs:
  %s = catchswitch within none [label %catch] unwind to caller
catch:
  %cp = catchpad within %s [ptr null]
  call void @catches() [ "funclet"(token %cp) ]
  catchret from %cp to label %ret
}

define internal void @callee() {
entry:
  call void @quiet()
  ret void
}

define void @caller(ptr %p) personality ptr @__gxx_personality_seha0 {
; CHECK-LABEL: define void @caller(
; CHECK:         invoke void @quiet()
; CHECK-NEXT:      to label %{{.*}} unwind label %cleanup
entry:
  invoke void @callee() to label %cont unwind label %cleanup
cont:
  ret void
cleanup:
  %pad = cleanuppad within none []
  call void @dtor(ptr %p) [ "funclet"(token %pad) ]
  cleanupret from %pad unwind to caller
}

; A function is inlined into one of its own mode only, which its personality
; names.
define internal void @sync(ptr %p) personality ptr @__gxx_personality_seh0 {
entry:
  invoke void @quiet() to label %cont unwind label %cleanup
cont:
  ret void
cleanup:
  %pad = cleanuppad within none []
  call void @dtor(ptr %p) [ "funclet"(token %pad) ]
  cleanupret from %pad unwind to caller
}

define void @modes(ptr %p) personality ptr @__gxx_personality_seha0 {
; CHECK-LABEL: define void @modes(
; CHECK:         invoke void @sync(ptr %p)
entry:
  invoke void @sync(ptr %p) to label %cont unwind label %cleanup
cont:
  ret void
cleanup:
  %pad = cleanuppad within none []
  call void @dtor(ptr %p) [ "funclet"(token %pad) ]
  cleanupret from %pad unwind to caller
}

; Nothing unwinds out of a funclet: a call in an inlined cleanup stays a call.
define internal void @cleans(ptr %p) personality ptr @__gxx_personality_seha0 {
entry:
  invoke void @quiet() to label %cont unwind label %cleanup
cont:
  ret void
cleanup:
  %pad = cleanuppad within none []
  call void @dtor(ptr %p) [ "funclet"(token %pad) ]
  cleanupret from %pad unwind to caller
}

define void @funclet(ptr %p, ptr %q) personality ptr @__gxx_personality_seha0 {
; CHECK-LABEL: define void @funclet(
; CHECK:         invoke void @quiet()
; CHECK-NEXT:      to label %{{.*}} unwind label %[[INNER:.*]]
; CHECK:       [[INNER]]:
; CHECK-NEXT:    [[PAD:%.*]] = cleanuppad within none []
; CHECK-NEXT:    call void @dtor(ptr %p) [ "funclet"(token [[PAD]]) ]
; CHECK-NEXT:    cleanupret from [[PAD]] unwind label %cleanup
entry:
  invoke void @cleans(ptr %p) to label %cont unwind label %cleanup
cont:
  ret void
cleanup:
  %pad = cleanuppad within none []
  call void @dtor(ptr %q) [ "funclet"(token %pad) ]
  cleanupret from %pad unwind to caller
}

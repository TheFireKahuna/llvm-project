; Two scopes are open in %both, and neither is inside the other: each begins
; outside the other, and neither cleanuppad unwinds to the other.

declare void @dtor(ptr) nounwind
declare i32 @__gxx_personality_seha0(...)
declare void @llvm.seh.scope.begin()
declare void @llvm.seh.scope.end()

define void @crossed(i1 %c, ptr %p) personality ptr @__gxx_personality_seha0 {
entry:
  br i1 %c, label %a, label %b
a:
  invoke void @llvm.seh.scope.begin() to label %both unwind label %cleanup.a
b:
  invoke void @llvm.seh.scope.begin() to label %both unwind label %cleanup.b
both:
  store i32 0, ptr %p
  ret void
cleanup.a:
  %pad.a = cleanuppad within none []
  call void @dtor(ptr %p) [ "funclet"(token %pad.a) ]
  cleanupret from %pad.a unwind to caller
cleanup.b:
  %pad.b = cleanuppad within none []
  call void @dtor(ptr %p) [ "funclet"(token %pad.b) ]
  cleanupret from %pad.b unwind to caller
}

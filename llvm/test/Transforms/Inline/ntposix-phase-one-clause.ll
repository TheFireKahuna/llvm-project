; RUN: opt -passes=inline -S < %s | FileCheck %s

; On NT-POSIX a call site's phase-one clause joins every top-level cleanup
; inlined there, whatever that cleanup's body does: a cleanup that returns,
; one that never returns, and one whose exit was rewritten to the site's pad
; all end the search at a site that ends it, and keep their own clause at a
; site a search passes. An inlined abort funclet stays one.

target triple = "x86_64-pc-windows-ntposix"

declare void @may_throw()
declare void @drop() nounwind
declare void @exit_now() nounwind noreturn
declare void @abort() nounwind noreturn
declare i32 @rust_eh_personality(...)

define internal void @returns() personality ptr @rust_eh_personality {
entry:
  invoke void @may_throw() to label %done unwind label %cleanup
done:
  ret void
cleanup:
  %c = cleanuppad within none [i8 0]
  call void @drop() [ "funclet"(token %c) ]
  cleanupret from %c unwind to caller
}

define internal void @diverges() personality ptr @rust_eh_personality {
entry:
  invoke void @may_throw() to label %done unwind label %cleanup
done:
  ret void
cleanup:
  %c = cleanuppad within none [i8 0]
  call void @exit_now() [ "funclet"(token %c) ]
  unreachable
}

define internal void @sealed() nounwind personality ptr @rust_eh_personality {
entry:
  invoke void @may_throw() to label %done unwind label %cleanup
done:
  ret void
cleanup:
  %c = cleanuppad within none [i8 1]
  call void @exit_now() [ "funclet"(token %c) ]
  unreachable
}

define internal void @aborts() nounwind personality ptr @rust_eh_personality {
entry:
  invoke void @may_throw() to label %done unwind label %terminate
done:
  ret void
terminate:
  %t = cleanuppad within none [i8 2]
  call void @abort() [ "funclet"(token %t) ]
  unreachable
}

; At a site of a body that cannot unwind every inlined cleanup takes clause 1,
; and the returning one's exit is the site's abort funclet.
; CHECK-LABEL: define void @boundary(
; CHECK: cleanuppad within none [i8 1]
; CHECK-NEXT: call void @drop()
; CHECK-NEXT: cleanupret from %{{.*}} unwind label %terminate
; CHECK: cleanuppad within none [i8 1]
; CHECK-NEXT: call void @exit_now()
; CHECK: cleanuppad within none [i8 1]
; CHECK-NEXT: call void @exit_now()
; CHECK: cleanuppad within none [i8 2]
; CHECK: terminate:
; CHECK-NEXT: cleanuppad within none [i8 2]
; CHECK-NOT: [i8 0]
define void @boundary() nounwind personality ptr @rust_eh_personality {
entry:
  invoke void @returns() to label %a unwind label %terminate
a:
  invoke void @diverges() to label %b unwind label %terminate
b:
  invoke void @sealed() to label %c unwind label %terminate
c:
  invoke void @aborts() to label %done unwind label %terminate
done:
  ret void
terminate:
  %t = cleanuppad within none [i8 2]
  call void @abort() [ "funclet"(token %t) ]
  unreachable
}

; At a site a search passes each inlined cleanup keeps its own clause, a
; boundary inlined there included.
; CHECK-LABEL: define void @passing(
; CHECK: cleanuppad within none [i8 0]
; CHECK-NEXT: call void @drop()
; CHECK: cleanuppad within none [i8 0]
; CHECK-NEXT: call void @exit_now()
; CHECK: cleanuppad within none [i8 1]
; CHECK-NEXT: call void @exit_now()
; CHECK: cleanuppad within none [i8 2]
define void @passing() personality ptr @rust_eh_personality {
entry:
  invoke void @returns() to label %a unwind label %cleanup
a:
  invoke void @diverges() to label %b unwind label %cleanup
b:
  invoke void @sealed() to label %c unwind label %cleanup
c:
  invoke void @aborts() to label %done unwind label %cleanup
done:
  ret void
cleanup:
  %p = cleanuppad within none [i8 0]
  call void @drop() [ "funclet"(token %p) ]
  cleanupret from %p unwind to caller
}

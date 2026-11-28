; RUN: not llc -mtriple=x86_64-pc-linux-gnu < %s 2>&1 | FileCheck %s
; RUN: not llc -mtriple=x86_64-w64-windows-gnu -O0 < %s 2>&1 | FileCheck %s

; A funclet pad under a personality that takes landing pads is rejected.

; CHECK: LLVM ERROR: funclet pad in function 'f' under a landing-pad personality

declare void @g()
declare i32 @__gxx_personality_v0(...)

define void @f() personality ptr @__gxx_personality_v0 {
  invoke void @g() to label %done unwind label %cleanup
done:
  ret void
cleanup:
  %pad = cleanuppad within none []
  call void @g() [ "funclet"(token %pad) ]
  cleanupret from %pad unwind to caller
}

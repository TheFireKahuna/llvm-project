; RUN: opt -mtriple=x86_64-windows-itanium -passes=thread-pointer -S %s \
; RUN:   | FileCheck %s

; Windows Itanium's unwinder calls a cleanup with no thread pointer of the
; function's, so a use of one in the cleanup takes its own from __tp_probe at
; the cleanup's head, never the function's register or a copy of it in the
; frame. A body passed its thread pointer keeps reading it in the register.

declare i32 @__gxx_personality_seh0(...)
declare void @g() "thread-pointer-argument"

; CHECK-LABEL: define void @f()
; CHECK:       %tp = call ptr @llvm.thread.pointer.p0()
; CHECK:       invoke void @g() [ "threadpointer"(ptr %tp) ]
; CHECK:       %pad = cleanuppad within none []
; CHECK-NEXT:  %[[PROBE:[0-9]+]] = call preserve_allcc ptr @__tp_probe() [ "funclet"(token %pad) ]
; CHECK-NEXT:  call void @g() [ "funclet"(token %pad), "threadpointer"(ptr %[[PROBE]]) ]
; CHECK:       declare dllimport preserve_allcc ptr @__tp_probe() #[[RT:[0-9]+]]
; CHECK:       attributes #[[RT]] = { cold nounwind }
define void @f() "thread-pointer-argument" personality ptr @__gxx_personality_seh0 {
entry:
  invoke void @g() to label %done unwind label %cleanup
done:
  ret void
cleanup:
  %pad = cleanuppad within none []
  call void @g() [ "funclet"(token %pad) ]
  cleanupret from %pad unwind to caller
}

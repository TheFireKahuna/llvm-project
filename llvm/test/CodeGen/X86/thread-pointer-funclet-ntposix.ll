; RUN: opt -mtriple=x86_64-windows-ntposix -passes=thread-pointer -S %s | FileCheck %s
; RUN: llc -mtriple=x86_64-windows-ntposix < %s | FileCheck %s --check-prefix=X64
; RUN: %if aarch64-registered-target %{ llc -mtriple=aarch64-windows-ntposix < %s \
; RUN:   | FileCheck %s --check-prefix=A64 %}

; NT-POSIX's unwind driver calls a cleanup with the thread pointer in its
; register, so a cleanup of a function passed one reads it there: no probe,
; no derivation and no reload from the frame, as thread-pointer-funclet.ll
; shows Windows Itanium's probe. A function's plain entry, a body of its own,
; unwinds through nothing of its own and derives inline whatever the
; function's personality.

declare i32 @__C_specific_handler(...)
declare void @g() "thread-pointer-argument"

; CHECK-LABEL: define void @seh()
; CHECK:       %pad = cleanuppad within none []
; CHECK-NEXT:  %[[TP:[a-z0-9.]+]] = call ptr @llvm.thread.pointer.p0() [ "funclet"(token %pad) ]
; CHECK-NEXT:  call void @g() [ "funclet"(token %pad), "threadpointer"(ptr %[[TP]]) ]
; CHECK-NOT:   __tp_probe

; X64-LABEL:   "?dtor$2@?0?seh@4HA":
; X64-NOT:     %r14
; X64:         callq g$tp
; X64-NOT:     %r14
; X64:         retq
; X64-LABEL:   seh:
; X64:         movl %gs:72,
; X64-NOT:     __tp_slow
; X64:         callq seh$tp
; X64-NOT:     __tp_probe

; A64-LABEL:   "?dtor$2@?0?seh@4HA":
; A64-NOT:     x28
; A64:         bl g$tp
; A64-NOT:     x28
; A64:         ret
; A64-LABEL:   seh:
; A64:         ldr w{{[0-9]+}}, [x18, #72]
; A64-NOT:     __tp_slow
; A64:         bl seh$tp
; A64-NOT:     __tp_probe
define void @seh() "thread-pointer-argument" personality ptr @__C_specific_handler {
entry:
  invoke void @g() to label %done unwind label %cleanup
done:
  ret void
cleanup:
  %pad = cleanuppad within none []
  call void @g() [ "funclet"(token %pad) ]
  cleanupret from %pad unwind to caller
}

; RUN: opt -passes=simplifycfg -S < %s | FileCheck %s

; Where exception tables cover every instruction, the llvm.seh.* markers of an
; EH scope are invokes of functions that do not throw, and stay invokes: an
; asynchronous exception takes their unwind edge.

target triple = "x86_64-unknown-windows-itanium"

declare void @dtor(ptr) nounwind
declare i32 @__gxx_personality_seha0(...)
declare void @llvm.seh.scope.begin()
declare void @llvm.seh.scope.end()

; A scope whose cleanup is empty goes with its markers, which would otherwise
; close the scope around it once they unwound to that scope's cleanuppad; the
; markers of the scope around it stay.
define void @empty(ptr %p) personality ptr @__gxx_personality_seha0 {
; CHECK-LABEL: define void @empty(
; CHECK:         invoke void @llvm.seh.scope.begin()
; CHECK-NEXT:      to label %outer unwind label %cleanup
; CHECK:       outer:
; CHECK-NEXT:    store i32 1, ptr %p
; CHECK-NEXT:    store i32 2, ptr %p
; CHECK-NEXT:    invoke void @llvm.seh.scope.end()
; CHECK-NEXT:      to label %out unwind label %cleanup
; CHECK-NOT:     cleanup.inner
entry:
  invoke void @llvm.seh.scope.begin() to label %outer unwind label %cleanup
outer:
  store i32 1, ptr %p
  invoke void @llvm.seh.scope.begin() to label %inner unwind label %cleanup.inner
inner:
  store i32 2, ptr %p
  invoke void @llvm.seh.scope.end() to label %done unwind label %cleanup.inner
done:
  invoke void @llvm.seh.scope.end() to label %out unwind label %cleanup
out:
  ret void
cleanup.inner:
  %pad.inner = cleanuppad within none []
  cleanupret from %pad.inner unwind label %cleanup
cleanup:
  %pad = cleanuppad within none []
  call void @dtor(ptr %p) [ "funclet"(token %pad) ]
  cleanupret from %pad unwind to caller
}

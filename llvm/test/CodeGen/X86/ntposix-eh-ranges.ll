; RUN: llc -O2 -verify-machineinstrs -mtriple=x86_64-pc-windows-ntposix < %s | FileCheck %s
; RUN: llc -O2 -verify-machineinstrs -mtriple=x86_64-pc-windows-itanium < %s | FileCheck %s

; Native Windows frame metadata must retain the Itanium call-site ranges.
; CHECK-LABEL: cleanup:
; CHECK: .seh_handler rust_eh_personality, @unwind, @except
; CHECK: [[BEGIN:.Ltmp[0-9]+]]:
; CHECK-NEXT: callq may_throw
; CHECK: [[END:.Ltmp[0-9]+]]:
; CHECK: [[PAD:.Ltmp[0-9]+]]:
; CHECK: callq cleanup_effect
; CHECK: .uleb128 [[BEGIN]]-[[FUNC:.Lfunc_begin[0-9]+]]
; CHECK-NEXT: .uleb128 [[END]]-[[BEGIN]]
; CHECK-NEXT: .uleb128 [[PAD]]-[[FUNC]]

declare void @may_throw()
declare void @cleanup_effect() nounwind
declare i32 @rust_eh_personality(...)

define void @cleanup() personality ptr @rust_eh_personality {
entry:
  invoke void @may_throw() to label %normal unwind label %exception
normal:
  ret void
exception:
  %value = landingpad { ptr, i32 } cleanup
  call void @cleanup_effect()
  resume { ptr, i32 } %value
}

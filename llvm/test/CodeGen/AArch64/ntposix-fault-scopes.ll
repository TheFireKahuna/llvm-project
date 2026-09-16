; RUN: llc -O2 -verify-machineinstrs -mtriple=aarch64-pc-windows-ntposix < %s | FileCheck %s
; RUN: llc -O0 -verify-machineinstrs -mtriple=aarch64-pc-windows-ntposix < %s | FileCheck %s
; RUN: llc -O0 -verify-machineinstrs -mtriple=aarch64-pc-windows-ntposix -fast-isel < %s | FileCheck %s
; RUN: llc -O0 -verify-machineinstrs -mtriple=aarch64-pc-windows-ntposix -global-isel=false -fast-isel=false < %s | FileCheck %s

; Under a landingpad personality and the eh-asynch module flag, an invoke of
; llvm.seh.scope.begin names the landing pad a fault anywhere after it must
; reach: every block that may fault under it gets a call-site range naming
; that pad, closed before the block's own invoke range. The marker emits no
; code, and a scope.end clears the pad.

declare void @may_throw()
declare void @cleanup_effect() nounwind
declare i32 @rust_eh_personality(...)
declare void @llvm.seh.scope.begin()
declare void @llvm.seh.scope.end()

; CHECK-LABEL: scoped:
; CHECK: .seh_handler rust_eh_personality, @unwind, @except
; CHECK-NOT: seh.scope
; CHECK: [[LOAD_BEGIN:.Ltmp[0-9]+]]:
; CHECK: ldr x{{[0-9]+}}, [x{{[0-9]+}}]
; CHECK: [[LOAD_END:.Ltmp[0-9]+]]:
; CHECK: [[CALL_BEGIN:.Ltmp[0-9]+]]:
; CHECK-NEXT: bl may_throw
; CHECK: [[CALL_END:.Ltmp[0-9]+]]:
; CHECK: [[PAD:.Ltmp[0-9]+]]:
; CHECK: bl cleanup_effect
; CHECK: GCC_except_table
; CHECK: .uleb128 [[LOAD_BEGIN]]-[[FUNC:.Lfunc_begin[0-9]+]]
; CHECK-NEXT: .uleb128 [[CALL_END]]-[[LOAD_BEGIN]]
; CHECK-NEXT: .uleb128 [[PAD]]-[[FUNC]]
define i64 @scoped(ptr %p) personality ptr @rust_eh_personality {
entry:
  invoke void @llvm.seh.scope.begin() to label %body unwind label %exception
body:
  %v = load volatile i64, ptr %p
  invoke void @may_throw() to label %normal unwind label %exception
normal:
  ret i64 %v
exception:
  %value = landingpad { ptr, i32 } cleanup
  call void @cleanup_effect()
  resume { ptr, i32 } %value
}

; A block after scope.end is a gap again: its load gets no range, and the
; table holds the one entry for the call.
; CHECK-LABEL: ended:
; CHECK-NOT: .Ltmp{{[0-9]+}}:
; CHECK: ldr x{{[0-9]+}}, [x{{[0-9]+}}]
; CHECK: [[EBEGIN:.Ltmp[0-9]+]]:
; CHECK-NEXT: bl may_throw
; CHECK: [[EEND:.Ltmp[0-9]+]]:
; CHECK: GCC_except_table
; CHECK: .uleb128 [[EBEGIN]]-[[EFUNC:.Lfunc_begin[0-9]+]]
; CHECK-NEXT: .uleb128 [[EEND]]-[[EBEGIN]]
define i64 @ended(ptr %p) personality ptr @rust_eh_personality {
entry:
  invoke void @llvm.seh.scope.begin() to label %opened unwind label %exception
opened:
  invoke void @llvm.seh.scope.end() to label %body unwind label %exception
body:
  %v = load volatile i64, ptr %p
  invoke void @may_throw() to label %normal unwind label %exception
normal:
  ret i64 %v
exception:
  %value = landingpad { ptr, i32 } cleanup
  call void @cleanup_effect()
  resume { ptr, i32 } %value
}

!llvm.module.flags = !{!0}
!0 = !{i32 1, !"eh-asynch", i32 1}

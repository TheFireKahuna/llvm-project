; RUN: llc -O2 -verify-machineinstrs -mtriple=x86_64-pc-windows-ntposix < %s | FileCheck %s
; RUN: llc -O0 -verify-machineinstrs -mtriple=x86_64-pc-windows-ntposix < %s | FileCheck %s

; A pad only fault-scope markers reach is a continuation target exactly when
; some block under the scope may fault and so got a range naming it. A scope
; over register arithmetic alone names nothing, and its pad, which nothing can
; land in, is left out of the EH continuation table.

declare void @cleanup_effect() nounwind
declare i32 @rust_eh_personality(...)
declare void @llvm.seh.scope.begin()

; CHECK-LABEL: scoped:
define i64 @scoped(ptr %p) personality ptr @rust_eh_personality {
entry:
  invoke void @llvm.seh.scope.begin() to label %body unwind label %exception
body:
  %v = load volatile i64, ptr %p
  ret i64 %v
exception:
  %value = landingpad { ptr, i32 } cleanup
  call void @cleanup_effect()
  resume { ptr, i32 } %value
}

; CHECK-LABEL: quiet:
define i64 @quiet(i64 %a, i64 %b) personality ptr @rust_eh_personality {
entry:
  invoke void @llvm.seh.scope.begin() to label %body unwind label %exception
body:
  %v = add i64 %a, %b
  ret i64 %v
exception:
  %value = landingpad { ptr, i32 } cleanup
  call void @cleanup_effect()
  resume { ptr, i32 } %value
}

; CHECK: .section .gehcont$y
; CHECK-NEXT: .symidx $ehgcr_0_{{[0-9]+}}
; CHECK-NOT: .symidx

!llvm.module.flags = !{!0, !1}
!0 = !{i32 1, !"ehcontguard", i32 1}
!1 = !{i32 1, !"eh-asynch", i32 1}

; RUN: llc -mtriple=x86_64-w64-windows-gnu < %s | FileCheck %s
; RUN: %if aarch64-registered-target %{ llc -mtriple=aarch64-w64-windows-gnu < %s | FileCheck %s %}
; DEFINE: %{split} = -function-splitting=all -mfs-split-ehcode
; RUN: llc -mtriple=x86_64-w64-windows-gnu %{split} < %s \
; RUN:   | FileCheck %s --check-prefixes=CHECK,SPLIT
; RUN: %if aarch64-registered-target %{ llc -mtriple=aarch64-w64-windows-gnu %{split} < %s \
; RUN:   | FileCheck %s --check-prefixes=CHECK,SPLIT %}

; Under Windows EH the unwinder enters a landing pad by restoring a context
; whose instruction pointer is the pad, so with ehcontguard the pad is listed
; as an EH continuation target, once: the address the exception table names,
; also at the start of the cold section, which a nop keeps the pad off.

; CHECK-LABEL: f:
; SPLIT:       .uleb128 [[LP:.Ltmp[0-9]+]]-f.cold {{.*}} jumps to
; SPLIT:     f.cold:
; CHECK:       [[PAD:\$ehgcr_[0-9_]+]]:
; SPLIT-NEXT:  [[LP]]:
; CHECK:       .section .gehcont$y,"dr"
; CHECK-NEXT:  .symidx [[PAD]]
; CHECK-NOT:   .symidx
define void @f() personality ptr @__gxx_personality_seh0 {
  invoke void @g() to label %done unwind label %lpad
lpad:
  %lp = landingpad { ptr, i32 } cleanup
  call void @g()
  resume { ptr, i32 } %lp
done:
  ret void
}

declare void @g()
declare i32 @__gxx_personality_seh0(...)

!llvm.module.flags = !{!0}
!0 = !{i32 1, !"ehcontguard", i32 1}

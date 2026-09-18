; RUN: llc -O2 -verify-machineinstrs -mtriple=x86_64-pc-windows-ntposix < %s | FileCheck %s
; RUN: llc -O0 -verify-machineinstrs -mtriple=x86_64-pc-windows-ntposix < %s | FileCheck %s

; A landing pad only fault accesses reach is a continuation target like any
; other pad: the kernel validates the driver's landing there.

declare void @cleanup_effect() nounwind
declare i32 @rust_eh_personality(...)
declare i64 @llvm.fault.load.i64.p0(ptr, i32)

; CHECK-LABEL: load:
; CHECK: movq (%{{[a-z0-9]+}}), %{{[a-z0-9]+}}
; CHECK: $ehgcr_0_{{[0-9]+}}:
; CHECK: callq cleanup_effect
define i64 @load(ptr %p) personality ptr @rust_eh_personality {
entry:
  %v = invoke i64 @llvm.fault.load.i64.p0(ptr %p, i32 8) to label %normal unwind label %exception
normal:
  ret i64 %v
exception:
  %value = landingpad { ptr, i32 } cleanup
  call void @cleanup_effect()
  resume { ptr, i32 } %value
}

; CHECK: .section .gehcont$y
; CHECK-NEXT: .symidx $ehgcr_0_{{[0-9]+}}

!llvm.module.flags = !{!0}
!0 = !{i32 1, !"ehcontguard", i32 1}

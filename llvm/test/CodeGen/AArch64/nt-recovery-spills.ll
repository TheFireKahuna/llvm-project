; RUN: llc -O3 -verify-machineinstrs -experimental-nt-recovery-spills < %s | FileCheck %s

target triple = "aarch64-pc-windows-msvc"

declare void @work(ptr)

; CHECK-LABEL: empty:
; CHECK-NOT: stur
; CHECK-NOT: mov w{{[0-9]+}}, #1
; CHECK: // NT_RECOVERY_CAPTURE
; CHECK: bl work
; CHECK: mov w0, #1
define i32 @empty(ptr %buffer) #0 {
entry:
  callbr void asm sideeffect "// NT_RECOVERY_CAPTURE\0Aadr x16, ${1:l}\0Astr x16, [$0, #8]\0Amov x16, sp\0Astr x16, [$0, #16]", "r,!i,~{x16},~{memory}"(ptr %buffer)
          to label %normal [label %recovery]
normal:
  call void @work(ptr %buffer)
  ret i32 0
recovery:
  ret i32 1
}

; CHECK-LABEL: scalar:
; CHECK: str x1, [sp, #[[SLOT:[0-9]+]]]
; CHECK: // NT_RECOVERY_CAPTURE
; CHECK-NOT: ldr
; CHECK: bl work
; CHECK: ldr x0, [sp, #[[SLOT]]]
define i64 @scalar(ptr %buffer, i64 %value) #0 {
entry:
  callbr void asm sideeffect "// NT_RECOVERY_CAPTURE\0Aadr x16, ${1:l}\0Astr x16, [$0, #8]\0Amov x16, sp\0Astr x16, [$0, #16]", "r,!i,~{x16},~{memory}"(ptr %buffer)
          to label %normal [label %recovery]
normal:
  call void @work(ptr %buffer)
  ret i64 0
recovery:
  ret i64 %value
}

attributes #0 = { "frame-pointer"="all" "target-features"="+v8.2a,+lse" }

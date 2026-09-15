; RUN: llc -O3 -verify-machineinstrs < %s | FileCheck %s
; RUN: llc -O3 -verify-machineinstrs -filetype=obj -o %t < %s
; RUN: llvm-readobj --symbols %t | FileCheck %s --check-prefix=SYMBOL
; SYMBOL: Name: {{.*}}ntrecovery0
; SYMBOL-NEXT: Value: {{[1-9][0-9]*}}

target triple = "aarch64-pc-windows-msvc"
declare void @llvm.experimental.nt.recovery.scope(ptr)
declare void @work(ptr)

; CHECK-LABEL: recover:
; CHECK: str x1,
; CHECK: // NT_RECOVERY_SCOPE
; CHECK: bl work
; CHECK: ldr x0,
; CHECK-NOT: .gljmp$y
; CHECK: .section .gehcont$y
; CHECK: .symidx
; CHECK-NOT: .symidx
; CHECK-NOT: .gljmp$y
define i64 @recover(ptr %buffer, i64 %value) #0 {
entry:
  callbr void @llvm.experimental.nt.recovery.scope(ptr %buffer)
      to label %normal [label %recovery]
normal:
  call void @work(ptr %buffer)
  ret i64 0
recovery:
  ret i64 %value
}
attributes #0 = { "frame-pointer"="all" }
!llvm.module.flags = !{!0, !1}
!0 = !{i32 2, !"cfguard", i32 2}
!1 = !{i32 2, !"ehcontguard", i32 1}

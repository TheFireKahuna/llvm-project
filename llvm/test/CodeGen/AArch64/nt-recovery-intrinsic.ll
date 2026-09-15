; RUN: llc -O3 -verify-machineinstrs < %s | FileCheck %s
; RUN: llc -O3 -verify-machineinstrs -filetype=obj -o %t < %s
; RUN: llvm-readobj --symbols %t | FileCheck %s --check-prefix=SYMBOL
; SYMBOL: Name: {{.*}}ntrecovery0
; SYMBOL-NEXT: Value: {{[1-9][0-9]*}}

target triple = "aarch64-pc-windows-msvc"
declare void @llvm.experimental.nt.recovery(ptr)
declare void @work(ptr)

; CHECK-LABEL: recover:
; CHECK: str x1, [sp, #[[SLOT:[0-9]+]]]
; CHECK: // NT_RECOVERY_BUILTIN
; CHECK-NOT: ldr
; CHECK: bl work
; CHECK: ldr x0, [sp, #[[SLOT]]]
; CHECK: .section .gljmp$y
; CHECK: .symidx
define i64 @recover(ptr %buffer, i64 %value) #0 {
entry:
  callbr void @llvm.experimental.nt.recovery(ptr %buffer)
      to label %normal [label %recovery]
normal:
  call void @work(ptr %buffer)
  ret i64 0
recovery:
  ret i64 %value
}

attributes #0 = { "frame-pointer"="all" "target-cpu"="generic" }
!llvm.module.flags = !{!0}
!0 = !{i32 2, !"cfguard", i32 2}

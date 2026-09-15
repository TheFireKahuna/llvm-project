; RUN: llc -O3 -verify-machineinstrs < %s | FileCheck %s
; RUN: llc -O3 -verify-machineinstrs -filetype=obj -o %t < %s
; RUN: llvm-readobj --symbols %t | FileCheck %s --check-prefix=SYMBOL
; SYMBOL: Name: {{.*}}ntrecovery0
; SYMBOL-NEXT: Value: {{[1-9][0-9]*}}

target triple = "x86_64-pc-windows-msvc"
declare void @llvm.experimental.nt.recovery.scope(ptr)
declare x86_64_sysvcc void @work(ptr)

; CHECK-LABEL: recover:
; CHECK: movq %rsi, [[SLOT:-?[0-9]+]](%rbp)
; CHECK: # NT_RECOVERY_SCOPE
; CHECK-NOT: movq [[SLOT]](%rbp)
; CHECK: callq work
; CHECK: movq [[SLOT]](%rbp), %rax
; CHECK-NOT: .gljmp$y
; CHECK: .section .gehcont$y
; CHECK: .symidx
; CHECK-NOT: .symidx
; CHECK-NOT: .gljmp$y
define x86_64_sysvcc i64 @recover(ptr %buffer, i64 %value) #0 {
entry:
  callbr void @llvm.experimental.nt.recovery.scope(ptr %buffer)
      to label %normal [label %recovery]
normal:
  call x86_64_sysvcc void @work(ptr %buffer)
  ret i64 0
recovery:
  ret i64 %value
}
attributes #0 = { "frame-pointer"="all" "target-cpu"="x86-64-v3" }
!llvm.module.flags = !{!0, !1}
!0 = !{i32 2, !"cfguard", i32 2}
!1 = !{i32 2, !"ehcontguard", i32 1}

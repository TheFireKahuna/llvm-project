; RUN: llc -O3 -verify-machineinstrs -experimental-nt-recovery-spills < %s | FileCheck %s

target triple = "x86_64-pc-windows-msvc"

declare x86_64_sysvcc void @work(ptr)

; A normal-only buffer needs no home; an empty recovery saves no extra GPRs.
; CHECK-LABEL: empty:
; CHECK-NOT: pushq %rbx
; CHECK-NOT: pushq %r12
; CHECK-NOT: movl $1
; CHECK: # NT_RECOVERY_CAPTURE
; CHECK-NOT: test
; CHECK: callq work
; CHECK: movl $1, %eax
define x86_64_sysvcc i32 @empty(ptr %buffer) #0 {
entry:
  callbr void asm sideeffect "# NT_RECOVERY_CAPTURE\0Aleaq ${1:l}(%rip), %rax\0Amovq %rax, 8($0)\0Amovq %rsp, 16($0)", "r,!i,~{rax},~{memory},~{dirflag},~{fpsr},~{flags}"(ptr %buffer)
          to label %normal [label %recovery]
normal:
  call x86_64_sysvcc void @work(ptr %buffer)
  ret i32 0
recovery:
  ret i32 1
}

; The recovery value is stored before capture and reloaded only on recovery.
; CHECK-LABEL: scalar:
; CHECK: movq %rsi, [[SLOT:-?[0-9]+]](%rbp)
; CHECK: # NT_RECOVERY_CAPTURE
; CHECK-NOT: movq [[SLOT]](%rbp)
; CHECK: callq work
; CHECK: movq [[SLOT]](%rbp), %rax
define x86_64_sysvcc i64 @scalar(ptr %buffer, i64 %value) #0 {
entry:
  callbr void asm sideeffect "# NT_RECOVERY_CAPTURE\0Aleaq ${1:l}(%rip), %rax\0Amovq %rax, 8($0)\0Amovq %rsp, 16($0)", "r,!i,~{rax},~{memory},~{dirflag},~{fpsr},~{flags}"(ptr %buffer)
          to label %normal [label %recovery]
normal:
  call x86_64_sysvcc void @work(ptr %buffer)
  ret i64 0
recovery:
  ret i64 %value
}

attributes #0 = { "frame-pointer"="all" "target-cpu"="x86-64-v3" }

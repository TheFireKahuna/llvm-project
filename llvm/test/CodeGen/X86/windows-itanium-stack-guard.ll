; RUN: llc -mtriple=x86_64-unknown-windows-itanium < %s | FileCheck %s --check-prefixes=CHECK,GLOBAL
; RUN: llc -mtriple=x86_64-pc-windows-ntposix < %s | FileCheck %s --check-prefixes=CHECK,GLOBAL
; RUN: sed 's/^;TLS //' %s | llc -mtriple=x86_64-unknown-windows-itanium | FileCheck %s --check-prefixes=CHECK,TLS
; RUN: sed 's/^;TLS //' %s | llc -mtriple=x86_64-pc-windows-ntposix | FileCheck %s --check-prefixes=CHECK,TLS

; On Windows Itanium and NT-POSIX a protected function reads its image's own
; guard, __stack_chk_guard, in one RIP-relative load, and stores it XORed with
; the stack pointer, reading no thread pointer; the epilogue compares it again
; from memory. Under -mstack-protector-guard=tls, and in a function whose
; thread pointer is pinned, it reads the thread's guard at 0x28 from the
; thread pointer instead. A mismatch calls __stack_chk_fail.

; CHECK-LABEL:  f$tp:
; GLOBAL:         movq __stack_chk_guard(%rip), %[[G:[a-z]+]]
; GLOBAL-NEXT:    xorq %rsp, %[[G]]
; GLOBAL-NEXT:    movq %[[G]], [[SLOT:[0-9]+]](%rsp)
; GLOBAL:         callq use
; GLOBAL-NEXT:    movq [[SLOT]](%rsp), %[[S:[a-z]+]]
; GLOBAL-NEXT:    xorq %rsp, %[[S]]
; GLOBAL-NEXT:    cmpq __stack_chk_guard(%rip), %[[S]]
; GLOBAL-NEXT:    jne
; GLOBAL:         movq __imp___stack_chk_fail@LOAD_CALL(%rip), %rax
; GLOBAL-NEXT:    callq *%rax
; TLS:            movq 40(%r14), %[[T:[a-z]+]]
; TLS-NEXT:       movq %[[T]], [[SLOT:[0-9]+]](%rsp)
; TLS:            callq use
; TLS-NEXT:       movq 40(%r14), %[[T2:[a-z]+]]
; TLS-NEXT:       cmpq [[SLOT]](%rsp), %[[T2]]
; TLS:            callq __stack_chk_fail
define void @f() sspstrong "thread-pointer-argument" {
  %a = alloca [16 x i8]
  call void @use(ptr %a)
  ret void
}

; CHECK-LABEL:  pinned$tp:
; CHECK:          movq 40(%r14), %[[T:[a-z]+]]
; CHECK-NEXT:     movq %[[T]], [[SLOT:[0-9]+]](%rsp)
; CHECK:          callq use
; CHECK-NEXT:     movq 40(%r14), %[[T2:[a-z]+]]
; CHECK-NEXT:     cmpq [[SLOT]](%rsp), %[[T2]]
; CHECK:          callq __stack_chk_fail
define void @pinned() sspstrong "pin-thread-pointer" "thread-pointer-argument" {
  %a = alloca [16 x i8]
  call void @use(ptr %a)
  ret void
}

; CHECK-NOT: __security_cookie

declare void @use(ptr)

;TLS !llvm.module.flags = !{!0}
;TLS !0 = !{i32 1, !"stack-protector-guard", !"tls"}

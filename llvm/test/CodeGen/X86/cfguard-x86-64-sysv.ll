; RUN: llc < %s -mtriple=x86_64-pc-windows-msvc | FileCheck %s

; The __guard_dispatch_icall thunk takes its target in RAX. That is fixed by the
; Windows loader and is independent of the calling convention of the guarded
; call, so the cfguardtarget parameter must be pinned to RAX under the System V
; convention as well as under Win64.

define x86_64_sysvcc i32 @func_sysv_cf_dispatch(ptr %0, i32 %1, i32 %2, i32 %3) {
entry:
  %4 = call x86_64_sysvcc i32 %0(i32 %1, i32 %2, i32 %3)
  ret i32 %4
}
; CHECK-LABEL: func_sysv_cf_dispatch:
; CHECK:      movq %rdi, %rax
; CHECK-NEXT: movl %esi, %edi
; CHECK-NEXT: movl %edx, %esi
; CHECK-NEXT: movl %ecx, %edx
; CHECK-NEXT: callq *__guard_dispatch_icall_fptr(%rip)

; With every System V argument register occupied by a real argument, the guard
; target must still reach RAX rather than a stack slot.
define x86_64_sysvcc i64 @func_sysv_cf_dispatch_six_args(ptr %0, i64 %1, i64 %2, i64 %3, i64 %4, i64 %5, i64 %6) {
entry:
  %7 = call x86_64_sysvcc i64 %0(i64 %1, i64 %2, i64 %3, i64 %4, i64 %5, i64 %6)
  ret i64 %7
}
; CHECK-LABEL: func_sysv_cf_dispatch_six_args:
; CHECK:     movq %rdi, %rax
; CHECK-NOT: movq %rdi, {{[0-9]*}}(%rsp)
; CHECK:     callq *__guard_dispatch_icall_fptr(%rip)

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"cfguard", i32 2}

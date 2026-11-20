; RUN: llc < %s -mtriple=x86_64-pc-windows-msvc | FileCheck %s

; The x86-64 guard check function uses RAX, R10 and R11 as scratch, so the
; arguments of the checked call are not kept in them across the check.

; CHECK-LABEL: args_across_check:
; CHECK-NOT:     {{%rax|%r10|%r11}}
; CHECK:         callq *__guard_check_icall_fptr(%rip)
; CHECK-NOT:     {{%rax|%r10|%r11}}
; CHECK:         callq *%r14
define i32 @args_across_check(ptr %p, i64 %a, i64 %b, i64 %c) nounwind {
entry:
  %r = call i32 %p(i64 %c, i64 %b, i64 %a)
  ret i32 %r
}

!llvm.module.flags = !{!0, !1}
!0 = !{i32 2, !"cfguard", i32 2}
!1 = !{i32 2, !"cfguard-mechanism", i32 1}

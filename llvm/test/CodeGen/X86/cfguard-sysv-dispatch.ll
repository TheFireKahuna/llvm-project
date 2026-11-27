; RUN: llc < %s -mtriple=x86_64-pc-windows-msvc | FileCheck %s --check-prefixes=CHECK,MSVC --implicit-check-not=__guard_
; RUN: llc < %s -mtriple=x86_64-pc-windows-ntposix | FileCheck %s --check-prefixes=CHECK,NTPOSIX --implicit-check-not=__guard_

; The guard dispatch function takes the target in RAX, whichever convention
; the call uses, and R10 and R11 as scratch. A call that passes something else
; in RAX, such as a System V variadic call's vector count in AL or a System V
; Swift call's sret pointer, or that keeps R10 or R11 across the call, takes
; the check function instead.

; CHECK-LABEL: sysv:
; CHECK:         movq %rdi, %rax
; CHECK:         callq *__guard_dispatch_icall_fptr(%rip)
define x86_64_sysvcc i32 @sysv(ptr %p, i32 %a) nounwind {
entry:
  %r = call x86_64_sysvcc i32 %p(i32 %a, i32 %a)
  ret i32 %r
}

; CHECK-LABEL: sysv_varargs:
; CHECK:         callq *__guard_check_icall_fptr(%rip)
; CHECK:         movb $1, %al
; CHECK-NEXT:    callq *%rcx
define x86_64_sysvcc i32 @sysv_varargs(ptr %p, double %x) nounwind {
entry:
  %r = call x86_64_sysvcc i32 (i32, ...) %p(i32 1, double %x)
  ret i32 %r
}

; CHECK-LABEL: preserve_most:
; CHECK:         callq *__guard_check_icall_fptr(%rip)
; CHECK-NEXT:    callq *%r{{cx|di}}
define void @preserve_most(ptr %p) nounwind {
entry:
  call preserve_mostcc void %p()
  ret void
}

; CHECK-LABEL: swift_sret:
; MSVC:          movq %rcx, %rax
; MSVC-NEXT:     movq %rdx, %rcx
; MSVC-NEXT:     callq *__guard_dispatch_icall_fptr(%rip)
; NTPOSIX:       callq *__guard_check_icall_fptr(%rip)
; NTPOSIX-NEXT:  movq %rsi, %rax
; NTPOSIX-NEXT:  callq *%rdi
define void @swift_sret(ptr %p, ptr %r) nounwind {
entry:
  call swiftcc void %p(ptr sret(i64) %r)
  ret void
}

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"cfguard", i32 2}

; RUN: llc < %s -mtriple=x86_64-pc-windows-msvc | FileCheck %s --check-prefixes=CHECK,MSVC
; RUN: llc < %s -mtriple=x86_64-pc-windows-ntposix | FileCheck %s --check-prefixes=CHECK,NTPOSIX
; Control Flow Guard is currently only available on Windows

; The guard dispatch function takes the target in RAX, uses R10 and R11 as
; scratch, and leaves every other register to the target. A call whose
; convention passes something in RAX, does not put the target there, or keeps
; R10 or R11 across the call is guarded with the check function instead.

; CHECK-LABEL: preserve_most_varargs:
; CHECK:       callq *__guard_check_icall_fptr(%rip)
; CHECK-NOT:   __guard_dispatch_icall_fptr
define void @preserve_most_varargs(ptr %p) nounwind {
  call preserve_mostcc void (i32, ...) %p(i32 1, double 2.0)
  ret void
}

; CHECK-LABEL: preserve_most:
; CHECK:       callq *__guard_check_icall_fptr(%rip)
; CHECK-NOT:   __guard_dispatch_icall_fptr
define void @preserve_most(ptr %p) nounwind {
  call preserve_mostcc void %p()
  ret void
}

; CHECK-LABEL: regcall:
; CHECK:       callq *__guard_check_icall_fptr(%rip)
; CHECK-NOT:   __guard_dispatch_icall_fptr
define void @regcall(ptr %p, i64 %a) nounwind {
  call x86_regcallcc void %p(i64 %a)
  ret void
}

; CHECK-LABEL: preserve_none:
; CHECK:       callq *__guard_check_icall_fptr(%rip)
; CHECK-NOT:   __guard_dispatch_icall_fptr
define void @preserve_none(ptr %p, i64 %a) nounwind {
  call preserve_nonecc void %p(i64 %a)
  ret void
}

; CHECK-LABEL: intel_ocl_bi:
; CHECK:       callq *__guard_check_icall_fptr(%rip)
; CHECK-NOT:   __guard_dispatch_icall_fptr
define void @intel_ocl_bi(ptr %p, ptr %a) nounwind {
  call intel_ocl_bicc void %p(ptr %a)
  ret void
}

; Under the System V convention a Swift call passes its sret pointer in RAX.
; CHECK-LABEL: swift_sret:
; MSVC:        callq *__guard_dispatch_icall_fptr(%rip)
; NTPOSIX:     callq *__guard_check_icall_fptr(%rip)
; NTPOSIX-NOT: __guard_dispatch_icall_fptr
define swifttailcc void @swift_sret(ptr %p, ptr %s) nounwind {
  call swifttailcc void %p(ptr sret(i64) %s, i64 1)
  ret void
}

; CHECK-LABEL: swift:
; CHECK:       callq *__guard_dispatch_icall_fptr(%rip)
define swifttailcc void @swift(ptr %p, ptr %s) nounwind {
  call swifttailcc void %p(ptr %s, i64 1)
  ret void
}

; CHECK-LABEL: fastcall:
; CHECK:       callq *__guard_dispatch_icall_fptr(%rip)
define void @fastcall(ptr %p, i64 %a) nounwind {
  call x86_fastcallcc void %p(i64 %a)
  ret void
}

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"cfguard", i32 2}

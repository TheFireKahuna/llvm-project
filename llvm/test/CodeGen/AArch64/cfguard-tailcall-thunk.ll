; RUN: llc < %s -mtriple=aarch64-pc-windows-msvc | FileCheck %s
; RUN: sed -e 's/"cfguard", i32 2/"cfguard", i32 1/' %s | \
; RUN:   llc -mtriple=aarch64-pc-windows-msvc | FileCheck --check-prefix=NOCHECK %s
; RUN: llc < %s -mtriple=aarch64-pc-windows-msvc -filetype=obj -o /dev/null

; Under the check mechanism an indirect musttail call goes to a shared thunk
; with its target in x9, outside the argument registers, so the function
; making it calls nothing and needs no frame. The thunk keeps the frame record
; the check's call needs, with its unwind codes, which the object RUN line has
; MC check, and branches to the checked target in x15. An ordinary indirect
; call keeps the check before it.

; CHECK-LABEL: tail:
; CHECK-NOT:   stp
; CHECK:       mov x9, x1
; CHECK-NEXT:  b __llvm_guard_tailcall

; CHECK-LABEL: plain:
; CHECK:       adrp x8, __guard_check_icall_fptr
; CHECK:       blr x8

; CHECK:      .section .text,"xr",discard,__llvm_guard_tailcall
; CHECK:      __llvm_guard_tailcall:
; CHECK-NEXT: .seh_proc __llvm_guard_tailcall
; CHECK-NEXT: stp x29, x30, [sp, #-16]!
; CHECK-NEXT: .seh_save_fplr_x 16
; CHECK-NEXT: mov x29, sp
; CHECK-NEXT: .seh_set_fp
; CHECK-NEXT: .seh_endprologue
; CHECK-NEXT: mov x15, x9
; CHECK-NEXT: adrp x16, __guard_check_icall_fptr
; CHECK-NEXT: ldr x16, [x16, :lo12:__guard_check_icall_fptr]
; CHECK-NEXT: blr x16
; CHECK-NEXT: .seh_startepilogue
; CHECK-NEXT: ldp x29, x30, [sp], #16
; CHECK-NEXT: .seh_save_fplr_x 16
; CHECK-NEXT: .seh_endepilogue
; CHECK-NEXT: br x15
; CHECK-NEXT: .seh_endproc

; NOCHECK-LABEL: tail:
; NOCHECK:       br x1
; NOCHECK-NOT:   __llvm_guard_tailcall

define void @tail(i64 %a, ptr %target) {
  musttail call void %target(i64 %a, ptr %target)
  ret void
}

define void @plain(ptr %target) {
  call void %target()
  ret void
}

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"cfguard", i32 2}

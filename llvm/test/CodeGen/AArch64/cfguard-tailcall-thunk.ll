; RUN: llc < %s -mtriple=aarch64-pc-windows-msvc | FileCheck --check-prefixes=CHECK,TYPED %s
; RUN: sed -e 's/"cfguard", i32 2/"cfguard", i32 1/' %s | \
; RUN:   llc -mtriple=aarch64-pc-windows-msvc | FileCheck --check-prefix=NOCHECK %s
; RUN: llc < %s -mtriple=aarch64-pc-windows-msvc -O0 -global-isel \
; RUN:   | FileCheck --check-prefix=TYPED %s
; RUN: llc < %s -mtriple=aarch64-pc-windows-msvc -filetype=obj -o /dev/null

; Under the check mechanism an indirect musttail call goes to a shared thunk
; with its target in x9, outside the argument registers, so the function
; making it calls nothing and needs no frame. The thunk keeps the frame record
; the check's call needs, with its unwind codes, which the object RUN line has
; MC check, and branches to the checked target in x15. An ordinary indirect
; call keeps the check before it. A KCFI check of the call checks the target in
; x9 before the branch to the thunk.

; CHECK-LABEL: tail:
; CHECK-NOT:   stp
; CHECK:       mov x9, x1
; CHECK-NEXT:  b __llvm_guard_tailcall

;; Both instruction selectors.
; TYPED-LABEL: typed:
; TYPED:       mov x9, x1
; TYPED:       ldur w16, [x9, #-4]
; TYPED-NEXT:  movk w17, #24910
; TYPED-NEXT:  movk w17, #188, lsl #16
; TYPED-NEXT:  cmp w16, w17
; TYPED-NEXT:  b.eq [[PASS:.Ltmp[0-9]+]]
; TYPED-NEXT:  brk #0x8229
; TYPED-NEXT:  [[PASS]]:
; TYPED-NEXT:  b __llvm_guard_tailcall

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

define void @typed(i64 %a, ptr %target) {
  musttail call void %target(i64 %a, ptr %target) [ "kcfi"(i32 12345678) ]
  ret void
}

define void @plain(ptr %target) {
  call void %target()
  ret void
}

!llvm.module.flags = !{!0, !1}
!0 = !{i32 2, !"cfguard", i32 2}
!1 = !{i32 4, !"kcfi", i32 1}

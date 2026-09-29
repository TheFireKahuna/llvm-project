; RUN: llc -mtriple=aarch64-unknown-windows-itanium < %s | FileCheck %s
; RUN: llc -mtriple=aarch64-unknown-windows-itanium -global-isel < %s \
; RUN:   | FileCheck %s --check-prefix=CALL

;; With the kcfi-marker module flag, a call with a KCFI type goes through a
;; per-type check thunk, called with the target in X15 as the guard check
;; function is, a COMDAT that checks the type before the target and continues
;; into the guard check function, or on a mismatch into a weak alias of the
;; routine that fails fast if the target carries the marker.

; CHECK-LABEL: f1:
; CALL-LABEL:  f1:
; CHECK:         mov x15, x0
; CHECK-NEXT:    bl __llvm_kcfi_check_12345678
; CHECK-NEXT:    blr x0
; CALL:          bl __llvm_kcfi_check_12345678
; CALL-NEXT:     blr x{{[0-9]+}}
define void @f1(ptr %p) {
  call void %p() [ "kcfi"(i32 305419896) ]
  ret void
}

; CHECK:       .weak __llvm_kcfi_check_mismatch_12345678
; CHECK-NEXT:  __llvm_kcfi_check_mismatch_12345678 = __llvm_kcfi_check_open
; CHECK-NEXT:  .section .text,"xr",discard,__llvm_kcfi_check_12345678
; CHECK:       .globl __llvm_kcfi_check_12345678
; CHECK-NEXT:  .p2align 4
; CHECK-NEXT:  __llvm_kcfi_check_12345678:
; CHECK-NEXT:    ldur w16, [x15, #-4]
; CHECK-NEXT:    mov w17, #22136
; CHECK-NEXT:    movk w17, #4660, lsl #16
; CHECK-NEXT:    cmp w16, w17
; CHECK-NEXT:    b.ne __llvm_kcfi_check_mismatch_12345678
; CHECK-NEXT:    adrp x16, __guard_check_icall_fptr
; CHECK-NEXT:    ldr x16, [x16, :lo12:__guard_check_icall_fptr]
; CHECK-NEXT:    br x16
; CHECK-NEXT:  .section .text,"xr",discard,__llvm_kcfi_check_open
; CHECK:       .globl __llvm_kcfi_check_open
; CHECK-NEXT:  .p2align 4
; CHECK-NEXT:  __llvm_kcfi_check_open:
; CHECK-NEXT:    ldur x16, [x15, #-12]
; CHECK-NEXT:    mov x17, #7951
; CHECK-NEXT:    movk x17, #61312, lsl #16
; CHECK-NEXT:    movk x17, #44478, lsl #32
; CHECK-NEXT:    movk x17, #47326, lsl #48
; CHECK-NEXT:    cmp x16, x17
; CHECK-NEXT:    b.eq [[TRAP:.Ltmp[0-9]+]]
; CHECK-NEXT:    adrp x16, __guard_check_icall_fptr
; CHECK-NEXT:    ldr x16, [x16, :lo12:__guard_check_icall_fptr]
; CHECK-NEXT:    br x16
; CHECK-NEXT:  [[TRAP]]:
; CHECK-NEXT:    mov w0, #64
; CHECK-NEXT:    brk #0xf003

!llvm.module.flags = !{!0, !1}
!0 = !{i32 4, !"kcfi", i32 1}
!1 = !{i32 4, !"kcfi-marker", i32 -559038737}

; RUN: llc -mtriple=aarch64-unknown-windows-itanium < %s | FileCheck %s

;; A type the module opens both statically and dynamically takes the dynamic
;; routine and keeps its entries; a type opened only statically keeps the
;; static routine. Every routine is in a COMDAT of which the linker keeps the
;; largest, so that a dynamic opener anywhere in the image prevails.

; CHECK:       .section .text,"xr",largest,__llvm_kcfi_check_mismatch_00000001
; CHECK:         b __llvm_kcfi_check_open_dynamic
; CHECK-NEXT:    brk #0xf003
; CHECK:       .section .rdata$llvm_kcfi_00000001_m,"dr"
; CHECK-NEXT:  .p2align 3, 0x0
; CHECK-NEXT:  .xword __imp_imp1
; CHECK:       .section .text,"xr",largest,__llvm_kcfi_check_mismatch_00000002
; CHECK:         b __llvm_kcfi_check_open{{$}}
; CHECK-NEXT:  .section .rdata$llvm_kcfi_00000002_a
; CHECK:       .section .rdata$llvm_kcfi_00000002_m,"dr"
; CHECK-NEXT:  .p2align 3, 0x0
; CHECK-NEXT:  .xword __imp_imp2

declare !kcfi_type !3 !kcfi_import !2 dllimport void @imp1()
declare !kcfi_type !4 !kcfi_import !2 dllimport void @imp2()

define ptr @take1() {
  ret ptr @imp1
}

define ptr @take2() {
  ret ptr @imp2
}

!llvm.module.flags = !{!0, !1}
!kcfi.dynamic = !{!3}
!0 = !{i32 4, !"kcfi", i32 1}
!1 = !{i32 4, !"kcfi-marker", i32 -559038737}
!2 = !{}
!3 = !{i32 1}
!4 = !{i32 2}

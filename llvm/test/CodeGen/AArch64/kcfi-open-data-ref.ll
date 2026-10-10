; RUN: llc -mtriple=aarch64-unknown-windows-itanium < %s | FileCheck %s

;; A known import that static data refers to is listed by its import address
;; table entry alone, with no cell holding its thunk: the object's link-only
;; records ask the linker to list the thunk where static data holds it.

; CHECK:       .section .rdata$llvm_kcfi_12345678_a,"dr",discard,__llvm_kcfi_list_12345678
; CHECK-NEXT:  .p2align 3, 0x0
; CHECK-NEXT:  .globl __llvm_kcfi_list_12345678
; CHECK-NEXT:  __llvm_kcfi_list_12345678:
; CHECK-NEXT:  .xword 305419896
; CHECK-NEXT:  .section .rdata$llvm_kcfi_12345678_m,"dr"
; CHECK-NEXT:  .p2align 3, 0x0
; CHECK-NEXT:  .xword __imp_imp
; CHECK-NEXT:  .linkkcfilists
; CHECK-NEXT:  .section .rdata$llvm_kcfi_12345678_z,"dr",associative,__llvm_kcfi_list_12345678

@table = constant ptr @imp

declare !kcfi_type !3 !kcfi_import !2 dllimport void @imp()

!llvm.module.flags = !{!0, !1}
!0 = !{i32 4, !"kcfi", i32 1}
!1 = !{i32 4, !"function-type-prefix", i32 -559038737}
!2 = !{}
!3 = !{i32 305419896}

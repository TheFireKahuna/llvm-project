; RUN: llc -mtriple=aarch64-unknown-windows-itanium < %s | FileCheck %s

;; A known import that static data refers to is listed by its import address
;; table slot and by a read-only cell holding its own address, which resolves,
;; as the data's reference does, to its import thunk.

; CHECK:       .section .rdata$llvm_kcfi_12345678_a,"dr",discard,__llvm_kcfi_list_12345678
; CHECK:       .section .rdata,"dr"
; CHECK-NEXT:  .p2align 3, 0x0
; CHECK-NEXT:  [[CELL:.Ltmp[0-9]+]]:
; CHECK-NEXT:  .xword imp
; CHECK-NEXT:  .section .rdata$llvm_kcfi_12345678_m,"dr"
; CHECK-NEXT:  .p2align 3, 0x0
; CHECK-NEXT:  .xword __imp_imp
; CHECK-NEXT:  .xword [[CELL]]
; CHECK-NEXT:  .section .rdata$llvm_kcfi_12345678_z,"dr",associative,__llvm_kcfi_list_12345678

@table = constant ptr @imp

declare !kcfi_type !3 !kcfi_import !2 dllimport void @imp()

!llvm.module.flags = !{!0, !1}
!0 = !{i32 4, !"kcfi", i32 1}
!1 = !{i32 4, !"kcfi-marker", i32 -559038737}
!2 = !{}
!3 = !{i32 305419896}

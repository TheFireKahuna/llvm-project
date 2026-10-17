;; table entry alone, with no cell holding its thunk: the object's link-only
;; records ask the linker to list the thunk where static data holds it.

; CHECK:       .section .rdata$llvm_kcfi_12345678_a,"dr",discard,__llvm_kcfi_list_12345678
; CHECK-NEXT:  .p2align 3, 0x0
; CHECK-NEXT:  .globl __llvm_kcfi_list_12345678
; CHECK-NEXT:  __llvm_kcfi_list_12345678:
; CHECK-NEXT:  .quad 305419896
; CHECK-NEXT:  .section .rdata$llvm_kcfi_12345678_m,"dr"
; CHECK-NEXT:  .p2align 3, 0x0
; CHECK-NEXT:  .quad __imp_imp
; CHECK-NEXT:  .linkkcfilists

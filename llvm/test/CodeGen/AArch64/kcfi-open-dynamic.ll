; RUN: llc -mtriple=aarch64-unknown-windows-itanium < %s | FileCheck %s
; RUN: llc -mtriple=aarch64-unknown-windows-itanium -filetype=obj < %s \
; RUN:   | llvm-objdump -d - | FileCheck %s --check-prefix=BYTES

;; A module whose !kcfi.dynamic names a KCFI type opens it dynamically: its
;; mismatch routine passes the type's list, here with no entries of its own,
;; to the dynamic scanner, and ends with a brk, so that the linker keeps it in
;; preference to a static opener's.

; CHECK:       .section .text,"xr",largest,__llvm_kcfi_check_mismatch_12345678
; CHECK:       __llvm_kcfi_check_mismatch_12345678:
; CHECK-NEXT:    adrp x16, __llvm_kcfi_list_12345678+8
; CHECK-NEXT:    add x16, x16, :lo12:__llvm_kcfi_list_12345678+8
; CHECK-NEXT:    b __llvm_kcfi_check_open_dynamic
; CHECK-NEXT:    brk #0xf003
; CHECK-NEXT:  .section .rdata$llvm_kcfi_12345678_a,"dr",discard,__llvm_kcfi_list_12345678
; CHECK-NEXT:  .p2align 3, 0x0
; CHECK-NEXT:  .globl __llvm_kcfi_list_12345678
; CHECK-NEXT:  __llvm_kcfi_list_12345678:
; CHECK-NEXT:  .xword 305419896
; CHECK-NEXT:  .section .rdata$llvm_kcfi_12345678_z,"dr",associative,__llvm_kcfi_list_12345678
; CHECK-NEXT:  .p2align 3, 0x0
; CHECK-NEXT:  .xword 610839793

; BYTES-LABEL: <__llvm_kcfi_check_mismatch_12345678>:
; BYTES-NEXT:    0: 90000050 adrp x16,
; BYTES-NEXT:    4: 91002210 add x16, x16, #0x8
; BYTES-NEXT:    8: 14000000 b
; BYTES-NEXT:    c: d43e0060 brk #0xf003
; BYTES-EMPTY:

!llvm.module.flags = !{!0, !1}
!kcfi.dynamic = !{!2}
!0 = !{i32 4, !"kcfi", i32 1}
!1 = !{i32 4, !"function-type-prefix", i32 -559038737}
!2 = !{i32 305419896}

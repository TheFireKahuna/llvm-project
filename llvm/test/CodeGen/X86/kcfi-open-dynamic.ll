; RUN: llc -mtriple=x86_64-unknown-windows-itanium < %s | FileCheck %s
; RUN: llc -mtriple=x86_64-unknown-windows-itanium -filetype=obj < %s \
; RUN:   | llvm-objdump -dr - | FileCheck %s --check-prefix=BYTES

;; A module whose !kcfi.dynamic names a KCFI type opens it dynamically: its
;; mismatch routines pass the type's list, here with no entries of its own, to
;; the dynamic scanner, and end with an int3, so that the linker keeps them in
;; preference to a static opener's.

; CHECK:       .section .text,"xr",largest,__llvm_kcfi_mismatch_12345678
; CHECK:       __llvm_kcfi_mismatch_12345678:
; CHECK-NEXT:    leaq __llvm_kcfi_list_12345678+8(%rip), %r10
; CHECK-NEXT:    jmp __llvm_kcfi_open_dynamic
; CHECK-NEXT:    int3
; CHECK-NEXT:  .section .text,"xr",largest,__llvm_kcfi_check_mismatch_12345678
; CHECK:       __llvm_kcfi_check_mismatch_12345678:
; CHECK-NEXT:    leaq __llvm_kcfi_list_12345678+8(%rip), %r10
; CHECK-NEXT:    jmp __llvm_kcfi_check_open_dynamic
; CHECK-NEXT:    int3
; CHECK-NEXT:  .section .rdata$llvm_kcfi_12345678_a,"dr",discard,__llvm_kcfi_list_12345678
; CHECK-NEXT:  .p2align 3, 0x0
; CHECK-NEXT:  .globl __llvm_kcfi_list_12345678
; CHECK-NEXT:  __llvm_kcfi_list_12345678:
; CHECK-NEXT:  .quad 305419896
; CHECK-NEXT:  .section .rdata$llvm_kcfi_12345678_z,"dr",associative,__llvm_kcfi_list_12345678
; CHECK-NEXT:  .p2align 3, 0x0
; CHECK-NEXT:  .quad 610839793

; BYTES-LABEL: <__llvm_kcfi_mismatch_12345678>:
; BYTES-NEXT:    0: 4c 8d 15 08 00 00 00 leaq 0x8(%rip), %r10
; BYTES-NEXT:      0000000000000003: IMAGE_REL_AMD64_REL32 __llvm_kcfi_list_12345678
; BYTES-NEXT:    7: e9 00 00 00 00       jmp
; BYTES-NEXT:      0000000000000008: IMAGE_REL_AMD64_REL32 __llvm_kcfi_open_dynamic
; BYTES-NEXT:    c: cc                   int3
; BYTES-EMPTY:

!llvm.module.flags = !{!0, !1}
!kcfi.dynamic = !{!2}
!0 = !{i32 4, !"kcfi", i32 1}
!1 = !{i32 4, !"function-type-prefix", i32 -559038737}
!2 = !{i32 305419896}

; RUN: llc -mtriple=aarch64-unknown-windows-itanium < %s | FileCheck %s --check-prefix=ASM
; RUN: llc -mtriple=aarch64-unknown-windows-itanium -filetype=obj < %s \
; RUN:   | llvm-objdump -s -j .text - | FileCheck %s --check-prefix=OBJ

;; With the kcfi-marker module flag, every typed function gets a prefix whose
;; 8 bytes before the type are 0F 1F 80, the marker, and B8, as on x86-64,
;; whether or not the module has the kcfi flag. The symbol that marks the
;; prefix is local, as on x86-64 COFF.

; ASM-NOT:   .globl __cfi_
; ASM-LABEL: __cfi_f1:
; ASM-NEXT:    .ascii "\017\037\200"
; ASM-NEXT:    .word 305419896
; ASM-NEXT:    .byte 184
; ASM-NEXT:    .word 12345678
; ASM-NEXT:  f1:
define void @f1() !kcfi_type !1 {
  ret void
}

;; An untyped function has no prefix.
; ASM-NOT:   __cfi_f2:
; ASM:       f2:
define void @f2() {
  ret void
}

; OBJ:      0000 0f1f8078 563412b8 4e61bc00 c0035fd6
; OBJ-NEXT: 0010 c0035fd6

!llvm.module.flags = !{!0}
!0 = !{i32 4, !"kcfi-marker", i32 305419896}
!1 = !{i32 12345678}

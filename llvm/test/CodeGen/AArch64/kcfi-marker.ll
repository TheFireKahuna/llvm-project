; RUN: llc -mtriple=aarch64-unknown-windows-itanium < %s | FileCheck %s --check-prefix=ASM
; RUN: llc -mtriple=aarch64-unknown-windows-itanium -filetype=obj < %s \
; RUN:   | llvm-objdump -s -j .text - | FileCheck %s --check-prefix=OBJ

;; With the function-type-prefix module flag, every typed function gets a prefix
;; whose 8 bytes before the type are 0F 1F 80, the marker, and B8, as on x86-64,
;; whether or not the module has the kcfi flag. The symbol that marks the prefix
;; is local, as on x86-64 COFF. Nops before the prefix align the entry to 16
;; bytes, where Control Flow Guard can suppress an export.

; ASM-NOT:   .globl __cfi_
; ASM:         .p2align 4
; ASM-NEXT:    nop
; ASM-NEXT:  __cfi_f1:
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

;; A function with a second type carries it 16 bytes before its entry.
; ASM:         .p2align 4
; ASM-NEXT:  __cfi_f3:
; ASM-NEXT:    .word 7
; ASM-NEXT:    .ascii "\017\037\200"
; ASM-NEXT:    .word 305419896
; ASM-NEXT:    .byte 184
; ASM-NEXT:    .word 12345678
; ASM-NEXT:  f3:
define void @f3() !kcfi_type !1 !kcfi_vfn_type !2 {
  ret void
}

;; Patchable-function-prefix nops count toward the alignment.
; ASM:         .p2align 4
; ASM-NEXT:  __cfi_f4:
; ASM:         .word 12345678
; ASM-NEXT:  .Ltmp{{.*}}:
; ASM-NEXT:    nop
; ASM-NEXT:  f4:
define void @f4() "patchable-function-prefix"="1" !kcfi_type !1 {
  ret void
}

; OBJ:      0000 1f2003d5 0f1f8078 563412b8 4e61bc00
; OBJ-NEXT: 0010 c0035fd6 c0035fd6 1f2003d5 1f2003d5
; OBJ-NEXT: 0020 07000000 0f1f8078 563412b8 4e61bc00
; OBJ-NEXT: 0030 c0035fd6 1f2003d5 1f2003d5 1f2003d5
; OBJ-NEXT: 0040 0f1f8078 563412b8 4e61bc00 1f2003d5
; OBJ-NEXT: 0050 c0035fd6

!llvm.module.flags = !{!0}
!0 = !{i32 4, !"function-type-prefix", i32 305419896}
!1 = !{i32 12345678}
!2 = !{i32 7}

; RUN: llc -mtriple=aarch64-unknown-windows-itanium < %s | FileCheck %s
; RUN: llc -mtriple=aarch64-unknown-windows-itanium -filetype=obj < %s \
; RUN:   | llvm-readobj --symbols - | FileCheck %s --check-prefix=SYMS
; RUN: llc -mtriple=aarch64-unknown-windows-itanium -filetype=obj < %s \
; RUN:   | llvm-objdump -dr - | FileCheck %s --check-prefix=BYTES

;; A module that takes the address of a known import with a KCFI type opens
;; the type statically. It defines the type's mismatch routine, instead of the
;; weak alias its thunks would refer to, in a COMDAT of which the linker keeps
;; the largest, passing the type's list to the static scanner. The list's
;; head, in a COMDAT, and its trailer, kept with the head, enclose the
;; module's entries, the import address table slots of the imports. An import
;; that is only called, or not known to be an import, opens nothing. The
;; routine, only ever branched to, has an instruction's alignment.

; CHECK-NOT:   .weak __llvm_kcfi_check_mismatch_12345678
; CHECK:       .section .text,"xr",largest,__llvm_kcfi_check_mismatch_12345678
; CHECK:       .globl __llvm_kcfi_check_mismatch_12345678
; CHECK-NEXT:  .p2align 2
; CHECK-NEXT:  __llvm_kcfi_check_mismatch_12345678:
; CHECK-NEXT:    adrp x16, __llvm_kcfi_list_12345678+8
; CHECK-NEXT:    add x16, x16, :lo12:__llvm_kcfi_list_12345678+8
; CHECK-NEXT:    b __llvm_kcfi_check_open
; CHECK-NEXT:  .section .rdata$llvm_kcfi_12345678_a,"dr",discard,__llvm_kcfi_list_12345678
; CHECK-NEXT:  .p2align 3, 0x0
; CHECK-NEXT:  .globl __llvm_kcfi_list_12345678
; CHECK-NEXT:  __llvm_kcfi_list_12345678:
; CHECK-NEXT:  .xword 305419896
; CHECK-NEXT:  .section .rdata$llvm_kcfi_12345678_m,"dr"
; CHECK-NEXT:  .p2align 3, 0x0
; CHECK-NEXT:  .xword __imp_imp1
; CHECK-NEXT:  .xword __imp_imp2
; CHECK-NEXT:  .linkkcfilists
; CHECK-NEXT:  .section .rdata$llvm_kcfi_12345678_z,"dr",associative,__llvm_kcfi_list_12345678
; CHECK-NEXT:  .p2align 3, 0x0
; CHECK-NEXT:  .xword 610839793
; CHECK-NOT:   __llvm_kcfi_list_
; CHECK-NOT:   __imp_

;; The routine is defined, not a weak external.
; SYMS:      Name: __llvm_kcfi_check_mismatch_12345678
; SYMS-NEXT: Value: 0
; SYMS-NEXT: Section: .text (
; SYMS:      StorageClass: External (0x2)

;; The offset of the first entry is in the instructions.
; BYTES-LABEL: <__llvm_kcfi_check_mismatch_12345678>:
; BYTES-NEXT:    0: 90000050 adrp x16,
; BYTES-NEXT:      0000000000000000: IMAGE_REL_ARM64_PAGEBASE_REL21 __llvm_kcfi_list_12345678
; BYTES-NEXT:    4: 91002210 add x16, x16, #0x8
; BYTES-NEXT:      0000000000000004: IMAGE_REL_ARM64_PAGEOFFSET_12A __llvm_kcfi_list_12345678
; BYTES-NEXT:    8: 14000000 b
; BYTES-NEXT:      0000000000000008: IMAGE_REL_ARM64_BRANCH26 __llvm_kcfi_check_open
; BYTES-EMPTY:

declare !kcfi_type !3 !kcfi_import !2 dllimport void @imp1()
declare !kcfi_type !3 !kcfi_import !2 dllimport void @imp2()
declare !kcfi_type !4 !kcfi_import !2 dllimport void @called()
declare !kcfi_type !4 dllimport void @unknown()

define ptr @take1() {
  call void @called()
  ret ptr @imp1
}

define ptr @take2() {
  ret ptr @imp2
}

define ptr @take3() {
  ret ptr @unknown
}

define void @call(ptr %p) {
  call void %p() [ "kcfi"(i32 305419896) ]
  ret void
}

!llvm.module.flags = !{!0, !1}
!0 = !{i32 4, !"kcfi", i32 1}
!1 = !{i32 4, !"kcfi-marker", i32 -559038737}
!2 = !{}
!3 = !{i32 305419896}
!4 = !{i32 7}

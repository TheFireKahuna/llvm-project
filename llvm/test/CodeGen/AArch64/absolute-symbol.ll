; RUN: llc < %s -mtriple=aarch64-linux-gnu | FileCheck %s
; RUN: llc < %s -mtriple=aarch64-linux-gnu -relocation-model=pic | FileCheck %s
; RUN: llc < %s -mtriple=aarch64-linux-gnu -code-model=tiny | FileCheck %s
; RUN: llc < %s -mtriple=aarch64-linux-gnu -code-model=large | FileCheck %s
; RUN: llc < %s -mtriple=aarch64-linux-gnu -O0 | FileCheck %s
; RUN: llc < %s -mtriple=aarch64-linux-gnu -global-isel -global-isel-abort=1 \
; RUN:   | FileCheck %s
; RUN: llc < %s -mtriple=aarch64-linux-gnu -fast-isel | FileCheck %s
; RUN: llc < %s -mtriple=aarch64-linux-gnu -filetype=obj | \
; RUN:   llvm-readobj -r - | FileCheck %s --check-prefix=RELOC
; RUN: llc < %s -mtriple=aarch64-unknown-windows-itanium | FileCheck %s
; RUN: llc < %s -mtriple=aarch64-unknown-windows-ntposix | FileCheck %s
; RUN: llc < %s -mtriple=aarch64-unknown-windows-itanium -O0 | FileCheck %s
; RUN: llc < %s -mtriple=aarch64-unknown-windows-itanium -global-isel \
; RUN:   -global-isel-abort=1 | FileCheck %s
; RUN: llc < %s -mtriple=aarch64-unknown-windows-itanium -filetype=obj | \
; RUN:   llvm-readobj -r - | FileCheck %s --check-prefix=COFF
; RUN: llc < %s -mtriple=aarch64-windows-msvc | FileCheck %s --check-prefix=MSVC

; An absolute symbol whose range fits in 32 bits is a constant the linker
; writes into MOVZ and MOVK, one instruction below 2^16 and two below 2^32.
; ADRP would compute it relative to the PC, and a GOT entry would load it.
; COFF has the relocations on Windows Itanium and NT-POSIX only.

@small = external global i8, !absolute_symbol !0
@word = external global i8, !absolute_symbol !1
@wide = external global i8, !absolute_symbol !2
@plain = external hidden global i8

define i64 @get_small() {
; CHECK-LABEL: get_small:
; CHECK:       movz x0, #:abs_g0:small
; CHECK-NEXT:  ret
  ret i64 ptrtoint (ptr @small to i64)
}

define i64 @get_word() {
; CHECK-LABEL: get_word:
; CHECK:       movz x0, #:abs_g1:word
; CHECK-NEXT:  movk x0, #:abs_g0_nc:word
; CHECK-NEXT:  ret
  ret i64 ptrtoint (ptr @word to i64)
}

define i1 @below_small(ptr %p) {
; CHECK-LABEL: below_small:
; CHECK:       movz [[REG:x[0-9]+]], #:abs_g0:small
; CHECK-NEXT:  {{cmp|subs x[0-9]+,}} x0, [[REG]]
  %c = icmp ult ptr %p, @small
  ret i1 %c
}

; An offset that would take the value past the range the MOVs write stays
; out of them.

@word_array = external global [32 x i8], !absolute_symbol !1

define i64 @get_word_offset() {
; CHECK-LABEL: get_word_offset:
; CHECK:       movz [[REG:x[0-9]+]], #:abs_g1:word_array{{$}}
; CHECK-NEXT:  movk [[REG]], #:abs_g0_nc:word_array{{$}}
; CHECK-NEXT:  add x0, [[REG]], #16
  ret i64 ptrtoint (ptr getelementptr (i8, ptr @word_array, i64 16) to i64)
}

; A symbol without a 32-bit range keeps its address form.

define ptr @get_wide() {
; CHECK-LABEL: get_wide:
; CHECK-NOT:   #:abs_g{{[01]}}:
; CHECK:       ret
  ret ptr @wide
}

define ptr @get_plain() {
; CHECK-LABEL: get_plain:
; CHECK-NOT:   #:abs_g{{[01]}}:
; CHECK:       ret
  ret ptr @plain
}

; RELOC:      R_AARCH64_MOVW_UABS_G0 small
; RELOC-NEXT: R_AARCH64_MOVW_UABS_G1 word
; RELOC-NEXT: R_AARCH64_MOVW_UABS_G0_NC word
; RELOC-NEXT: R_AARCH64_MOVW_UABS_G0 small
; RELOC-NEXT: R_AARCH64_MOVW_UABS_G1 word_array 0x0
; RELOC-NEXT: R_AARCH64_MOVW_UABS_G0_NC word_array 0x0

; COFF:      IMAGE_REL_LLVM_MOVW_UABS_G0 small
; COFF-NEXT: IMAGE_REL_LLVM_MOVW_UABS_G1 word
; COFF-NEXT: IMAGE_REL_LLVM_MOVW_UABS_G0_NC word
; COFF-NEXT: IMAGE_REL_LLVM_MOVW_UABS_G0 small
; COFF-NEXT: IMAGE_REL_LLVM_MOVW_UABS_G1 word_array
; COFF-NEXT: IMAGE_REL_LLVM_MOVW_UABS_G0_NC word_array

; MSVC-NOT: :abs_g

!0 = !{i64 0, i64 65536}
!1 = !{i64 0, i64 4294967296}
!2 = !{i64 0, i64 4294967297}

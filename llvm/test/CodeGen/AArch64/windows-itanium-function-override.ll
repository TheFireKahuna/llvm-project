; RUN: llc -mtriple=aarch64-unknown-windows-itanium < %s \
; RUN:   | FileCheck %s --check-prefix=ASM --implicit-check-not=resolver
; RUN: llc -mtriple=aarch64-pc-windows-ntposix -filetype=obj < %s -o %t.o
; RUN: llvm-readobj --section-data --relocs %t.o | FileCheck %s --check-prefix=OBJ

;; On Windows Itanium and NT-POSIX a multiversioned function is a stub: its
;; type prefix, then a branch to X, the version every processor the target
;; supports runs. Where each other version needs only features that Windows
;; numbers, sections in Microsoft's function-override convention, keyed on X,
;; hold a diagram that tests them in priority order and the versions it
;; selects among. A version is tested on the features it names with the ones
;; they imply, less the baseline's: I8MM is 0x10014, SHA3 0x10005 and 0x10006
;; with SHA2 within the baseline, and DotProd alone is X. A call of the
;; function is direct, under -fno-plt too.

; ASM-LABEL: caller:
; ASM:         bl f

; ASM:       .section .text,"xr",discard,f
; ASM-NEXT:  .globl f
; ASM:       .zero 4
; ASM-NEXT:  __cfi_f:
; ASM-NEXT:    .xword
; ASM-NEXT:    .word 12345678
; ASM-NEXT:  f:
; ASM-NEXT:    b f._Mdotprod
; ASM-NEXT:  .globl f._Mdotprod_$fo_default$
; ASM-NEXT:  f._Mdotprod_$fo_default$ = f._Mdotprod
; ASM-NEXT:  .section f._Mdotprod_$fo_bdd$,"y",discard,f._Mdotprod_$fo$
; ASM-NEXT:  .globl f._Mdotprod_$fo$
; ASM-NEXT:  f._Mdotprod_$fo$:
; ASM-NEXT:    .word 1
; ASM-NEXT:    .word 48
; ASM-NEXT:    .hword 2
; ASM-NEXT:    .hword 1
; ASM-NEXT:    .word 65556
; ASM-NEXT:    .hword 1
; ASM-NEXT:    .hword 1
; ASM-NEXT:    .word 0
; ASM-NEXT:    .hword 5
; ASM-NEXT:    .hword 3
; ASM-NEXT:    .word 65541
; ASM-NEXT:    .hword 5
; ASM-NEXT:    .hword 4
; ASM-NEXT:    .word 65542
; ASM-NEXT:    .hword 4
; ASM-NEXT:    .hword 4
; ASM-NEXT:    .word 1
; ASM-NEXT:    .xword 0
; ASM-NEXT:  .section f._Mdotprod_$fo_rvas$,"y",associative,f._Mdotprod_$fo$
; ASM-NEXT:    bl f._Mi8mm
; ASM-NEXT:    bl f._Msha3

;; Microsoft's ARM64 objects name each candidate by a BRANCH26.
; OBJ:      Section ({{.*}}) f._Mdotprod_$fo_rvas$ {
; OBJ-NEXT:   0x0 IMAGE_REL_ARM64_BRANCH26 f._Mi8mm
; OBJ-NEXT:   0x4 IMAGE_REL_ARM64_BRANCH26 f._Msha3
; OBJ-NEXT: }

@f = weak_odr ifunc i32 (), ptr @f.resolver, !kcfi_type !2

declare i1 @test()

define weak_odr hidden ptr @f.resolver() {
  %a = call i1 @test()
  %b = call i1 @test()
  %x = select i1 %a, ptr @f._Mi8mm, ptr @f._Mdotprod
  %y = select i1 %b, ptr @f._Msha3, ptr %x
  ret ptr %y
}

define hidden i32 @f._Msha3() "fmv-features"="sha3" {
  ret i32 2
}
define hidden i32 @f._Mi8mm() "fmv-features"="i8mm" {
  ret i32 1
}
define hidden i32 @f._Mdotprod() "fmv-features"="dotprod" {
  ret i32 0
}

define i32 @caller() {
  %a = call i32 @f()
  ret i32 %a
}

!llvm.module.flags = !{!0, !1, !3}
!0 = !{i32 4, !"kcfi", i32 1}
!1 = !{i32 1, !"function-type-prefix", i32 16}
!2 = !{i32 12345678}
!3 = !{i32 7, !"RtLibUseGOT", i32 1}

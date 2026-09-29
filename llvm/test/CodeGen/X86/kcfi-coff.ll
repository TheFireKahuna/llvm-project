; RUN: llc -mtriple=x86_64-pc-windows-msvc < %s | FileCheck %s --check-prefix=ASM
; RUN: llc -mtriple=x86_64-pc-windows-msvc -filetype=obj < %s \
; RUN:   | llvm-readobj --symbols - | FileCheck %s --check-prefix=OBJ

;; On COFF, the symbol that marks the type data is local, even for a COMDAT
;; function: a linker treats an external symbol that only a discarded copy
;; defines as undefined, and a prevailing copy may lack the type data.

; ASM:     .globl f1
; ASM-NOT: .globl __cfi_f1
; ASM:     __cfi_f1:
; ASM:     .globl f2
; ASM-NOT: .globl __cfi_f2
; ASM:     __cfi_f2:

; OBJ:      Name: __cfi_f1
; OBJ-NEXT: Value: 0
; OBJ-NEXT: Section: .text (
; OBJ-NEXT: BaseType: Null
; OBJ-NEXT: ComplexType: Null
; OBJ-NEXT: StorageClass: Static
; OBJ:      Name: __cfi_f2
; OBJ-NEXT: Value: 0
; OBJ-NEXT: Section: .text (
; OBJ-NEXT: BaseType: Null
; OBJ-NEXT: ComplexType: Null
; OBJ-NEXT: StorageClass: Static

define void @f1() !kcfi_type !1 {
  ret void
}

$f2 = comdat any
define linkonce_odr void @f2() comdat !kcfi_type !1 {
  ret void
}

!llvm.module.flags = !{!0}
!0 = !{i32 4, !"kcfi", i32 1}
!1 = !{i32 12345678}

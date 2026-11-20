;; On Windows Itanium and NT-POSIX every function definition states its size,
;; one without unwind data included, as ELF's .size does; function-size.s
;; pins how an object carries it.

; RUN: llc -mtriple=x86_64-unknown-windows-itanium < %s | FileCheck %s --check-prefix=ASM
; RUN: llc -mtriple=aarch64-unknown-windows-ntposix < %s | FileCheck %s --check-prefix=ASM
; RUN: llc -mtriple=x86_64-windows-msvc < %s | FileCheck %s --check-prefix=MSVC
; RUN: llc -mtriple=x86_64-windows-gnu < %s | FileCheck %s --check-prefix=MSVC
; RUN: llc -mtriple=i686-unknown-windows-itanium < %s | FileCheck %s --check-prefix=MSVC

; ASM:      leaf:
; ASM:      [[END:.Lfunc_end0]]:
; ASM-NEXT: .size leaf, [[END]]-leaf
; ASM-NOT:  .size

; MSVC-NOT: .size

define void @leaf() {
  ret void
}

@data = global i32 0

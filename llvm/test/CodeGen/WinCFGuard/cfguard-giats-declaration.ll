; RUN: llc < %s -mtriple=x86_64-pc-windows-msvc | FileCheck %s --check-prefix=MSVC
; RUN: llc < %s -mtriple=x86_64-w64-windows-gnu | FileCheck %s --check-prefix=MSVC
; RUN: llc < %s -mtriple=x86_64-unknown-windows-itanium | FileCheck %s --check-prefix=ITANIUM

; Only on Windows Itanium does an address-taken declaration that is not
; dllimport get a .giats entry for an __imp_ symbol the module names, since
; only there may a function's address be loaded from its import pointer
; without dllimport.

@__imp_f = external global ptr
@p = global ptr null

declare void @f()

define void @take() {
  store ptr @f, ptr @p
  %imp = load ptr, ptr @__imp_f
  store ptr %imp, ptr @p
  ret void
}

; MSVC:      .section .giats$y,"dr"
; MSVC-NEXT: .section .gljmp$y,"dr"

; ITANIUM:      .section .giats$y,"dr"
; ITANIUM-NEXT: .symidx __imp_f

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"cfguard", i32 2}

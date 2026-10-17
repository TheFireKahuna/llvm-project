; RUN: llc -mtriple=aarch64-pc-windows-msvc < %s | FileCheck %s --check-prefix=MSVC
; RUN: llc -mtriple=aarch64-w64-windows-gnu < %s | FileCheck %s --check-prefix=MSVC
; RUN: llc -mtriple=aarch64-unknown-windows-itanium < %s | \
; RUN:   FileCheck %s --check-prefix=ITANIUM

; Only Windows Itanium calls a DSO-local extern_weak function directly; the
; other Windows targets keep the classification of its address.

declare extern_weak dso_local void @w()

define void @c() {
; MSVC-LABEL: c:
; MSVC:       ldr x8, [x8, :lo12:w]
; MSVC-NEXT:  blr x8
; ITANIUM-LABEL: c:
; ITANIUM:       bl w
  call void @w()
  ret void
}

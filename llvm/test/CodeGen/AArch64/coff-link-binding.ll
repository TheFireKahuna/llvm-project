; RUN: llc -mtriple=aarch64-unknown-windows-itanium < %s | FileCheck %s
; RUN: llc -mtriple=aarch64-pc-windows-ntposix < %s | FileCheck %s
; RUN: llc -mtriple=aarch64-pc-windows-msvc < %s | FileCheck %s --check-prefix=NONE

;; An object for Windows Itanium or NT-POSIX lists, in its link records, each
;; external definition in a COMDAT that has default visibility, which a linker
;; may share along import paths, so that the linker takes any other as hidden.
;; An alias is not such a definition.

; CHECK:     .linkbindings
; CHECK-DAG: .linkbinding f, default
; CHECK-DAG: .linkbinding w, default
; CHECK-DAG: .linkbinding v, default
; CHECK-NOT: .linkbinding {{h|s|g|hf|a}},

; NONE-NOT: .linkbinding

$v = comdat any
$h = comdat any
$f = comdat any
$hf = comdat any
@v = linkonce_odr global i32 1, comdat
@w = weak_odr global i32 2
@h = linkonce_odr hidden global i32 3, comdat
@s = global i32 4
@a = linkonce_odr alias i32, ptr @v

define linkonce_odr void @f() comdat {
  ret void
}

define linkonce_odr hidden void @hf() comdat {
  ret void
}

define void @g() {
  ret void
}

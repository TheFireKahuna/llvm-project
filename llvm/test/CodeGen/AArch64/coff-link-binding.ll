; RUN: llc -mtriple=aarch64-unknown-windows-itanium < %s | FileCheck %s
; RUN: llc -mtriple=aarch64-pc-windows-ntposix < %s | FileCheck %s
; RUN: llc -mtriple=aarch64-pc-windows-msvc < %s | FileCheck %s --check-prefix=NONE
; RUN: llc -mtriple=aarch64-unknown-windows-itanium < %s | FileCheck %s --check-prefix=NOT

;; An object for Windows Itanium or NT-POSIX lists, in its link records, each
;; external definition in a COMDAT that has default visibility, which a linker
;; may share along import paths, so that the linker takes any other as hidden.
;; An external alias at a definition's address is another name for it, with a
;; visibility of its own; one inside it names a part of it, which the linker
;; shares with it. A weak alias is a weak external, not such a definition.

; CHECK:     .linkbindings
; CHECK-DAG: .linkbinding f, default
; CHECK-DAG: .linkbinding w, default
; CHECK-DAG: .linkbinding v, default
; CHECK-DAG: .linkbinding e, default

; NOT-NOT: .linkbinding {{(h|s|g|hf|a|eh|ei)}},
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
@e = alias i32, ptr @v
@eh = hidden alias i32, ptr @v
@ei = alias i8, getelementptr (i8, ptr @w2, i64 4)
$w2 = comdat any
@w2 = linkonce_odr global [2 x i32] [i32 1, i32 2], comdat

define linkonce_odr void @f() comdat {
  ret void
}

define linkonce_odr hidden void @hf() comdat {
  ret void
}

define void @g() {
  ret void
}

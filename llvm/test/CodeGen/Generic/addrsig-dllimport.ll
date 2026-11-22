; REQUIRES: aarch64-registered-target, x86-registered-target
; RUN: llc -mtriple=x86_64-pc-windows-msvc -addrsig -filetype=obj %s -o %t
; RUN: llvm-readobj --addrsig %t | FileCheck %s
; RUN: llvm-nm %t | FileCheck --check-prefix=NM %s
; RUN: llc -mtriple=aarch64-pc-windows-msvc -addrsig -filetype=obj %s -o %t
; RUN: llvm-readobj --addrsig %t | FileCheck %s
; RUN: llvm-nm %t | FileCheck --check-prefix=NM %s

; An imported function or datum whose address is taken is listed by the
; __imp_ pointer the code loads, adding no symbol; a call-only function is not.

; CHECK:      Addrsig [
; CHECK-NEXT:   Sym: __imp_f
; CHECK-NEXT:   Sym: __imp_x
; CHECK-NEXT: ]

; NM-NOT: {{ [fx]$}}

declare dllimport void @f()
declare dllimport void @called() local_unnamed_addr
@x = external dllimport global i32

define ptr @take(ptr %p) {
  call void @called()
  store ptr @x, ptr %p
  ret ptr @f
}

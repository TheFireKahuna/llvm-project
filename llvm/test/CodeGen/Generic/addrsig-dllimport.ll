; REQUIRES: aarch64-registered-target, x86-registered-target
; RUN: llc -mtriple=x86_64-pc-windows-msvc -addrsig -filetype=obj %s -o %t
; RUN: llvm-readobj --addrsig %t | FileCheck %s
; RUN: llvm-nm %t | FileCheck --check-prefix=NM %s
; RUN: llc -mtriple=aarch64-pc-windows-msvc -addrsig -filetype=obj %s -o %t
; RUN: llvm-readobj --addrsig %t | FileCheck %s
; RUN: llvm-nm %t | FileCheck --check-prefix=NM %s

; An imported function or datum whose address is taken is listed by its
; __imp_ pointer, also one that only data names; a call-only function and one
; nothing uses are not, and the second adds no symbol.

; CHECK:      Addrsig [
; CHECK-NEXT:   Sym: __imp_f
; CHECK-NEXT:   Sym: __imp_data
; CHECK-NEXT:   Sym: __imp_x
; CHECK-NEXT: ]

; NM-NOT: {{ [fx]$}}
; NM-NOT: unused

declare dllimport void @f()
declare dllimport void @called() local_unnamed_addr
declare dllimport void @data()
declare dllimport void @unused()
@x = external dllimport global i32
@table = global ptr @data

define ptr @take(ptr %p) {
  call void @called()
  store ptr @x, ptr %p
  ret ptr @f
}

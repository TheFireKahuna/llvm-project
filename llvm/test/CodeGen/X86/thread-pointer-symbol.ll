; RUN: llc -mtriple=x86_64-windows-ntposix < %s | FileCheck %s

; The symbol of a function that is passed its thread pointer, and of an alias
; of one, is its name followed by $tp, in a definition and in a reference; a
; literal name keeps its spelling.

; CHECK-LABEL: f$tp:
; CHECK:       callq ext$tp
; CHECK:       callq plain
; CHECK-LABEL: lit:
; CHECK:       a$tp = f$tp

@a = alias void (), ptr @f

declare void @ext() "thread-pointer-argument"
declare void @plain()

define void @f() "thread-pointer-argument" {
  call void @ext()
  call void @plain()
  ret void
}

define void @"\01lit"() "thread-pointer-argument" {
  ret void
}

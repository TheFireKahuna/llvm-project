; RUN: llc -mtriple=x86_64-unknown-windows-itanium < %s | FileCheck %s
; RUN: %if aarch64-registered-target %{ llc -mtriple=aarch64-pc-windows-ntposix < %s | FileCheck %s %}

;; An extern_weak declaration that names an alternate is a weak external whose
;; alternate is that symbol, which the module need not define or declare.

declare extern_weak void @f() "weak-alternate"="g"
declare extern_weak void @h()

@p = global [2 x ptr] [ptr @f, ptr @h]

; CHECK:      .weak f
; CHECK-NEXT: f = g
; CHECK-NEXT: .weak h
; CHECK-NOT:  h =

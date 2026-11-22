; RUN: llc -mtriple=x86_64-unknown-windows-itanium < %s | FileCheck %s
; RUN: %if aarch64-registered-target %{ llc -mtriple=aarch64-pc-windows-ntposix < %s | FileCheck %s %}

;; An extern_weak declaration that names an alternate is a weak external whose
;; alternate is that symbol, which the module need not define or declare.
;; Being a definition, it is exported where it is dllexport.

declare extern_weak void @f() "weak-alternate"="g"
declare extern_weak dllexport void @e() "weak-alternate"="g"
declare extern_weak void @h()

@p = global [3 x ptr] [ptr @f, ptr @e, ptr @h]

; CHECK:      .section .drectve
; CHECK-NEXT: .ascii " -export:e"
; CHECK-NEXT: .weak f
; CHECK-NEXT: f = g
; CHECK-NEXT: .weak e
; CHECK-NEXT: e = g
; CHECK-NEXT: .weak h
; CHECK-NOT:  h =

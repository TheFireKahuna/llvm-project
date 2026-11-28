; RUN: llc -mtriple=x86_64-windows-ntposix < %s | FileCheck %s

; The symbol of a function that is passed its thread pointer, and of an alias
; of one, is its name followed by $tp, in a definition and in a reference; a
; literal name keeps its spelling. An ifunc's resolver says it for the ifunc.

; CHECK-LABEL: f$tp:
; CHECK:       callq ext$tp
; CHECK:       callq plain
; CHECK-LABEL: lit:
; CHECK:       a$tp = f$tp
; CHECK-LABEL: g$tp:
; CHECK-NEXT:  jmp g.default$tp
; CHECK-LABEL: h:
; CHECK-NEXT:  jmp h.default

@a = alias void (), ptr @f
@g = weak_odr dso_local ifunc void (), ptr @g.resolver
@h = weak_odr dso_local ifunc void (), ptr @h.resolver

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

define hidden void @g.default() "thread-pointer-argument" "fmv-features" {
  ret void
}

define weak_odr hidden ptr @g.resolver() "thread-pointer-argument" !resolver_fallback !0 {
  ret ptr @g.default
}

define hidden void @h.default() "fmv-features" {
  ret void
}

define weak_odr hidden ptr @h.resolver() !resolver_fallback !1 {
  ret ptr @h.default
}

!0 = !{ptr @g.default}
!1 = !{ptr @h.default}

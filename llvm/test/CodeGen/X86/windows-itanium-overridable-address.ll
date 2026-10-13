; RUN: llc < %s -mtriple=x86_64-unknown-windows-itanium | FileCheck %s

; A function definition carrying "overridable-default" is an overridable
; default: its address is the program's one replacement, reached through its
; import pointer in every image, so an address-take, with or without an
; addend, is a load of the import pointer, while a call to it stays direct. A
; definition without the attribute keeps its direct address.

; CHECK-LABEL: take:
; CHECK:       movq __imp__Znwy(%rip), %rax
; CHECK-NEXT:  retq
define ptr @take() {
  ret ptr @_Znwy
}

; CHECK-LABEL: take8:
; CHECK:       movq __imp__Znwy(%rip), %rax
; CHECK-NEXT:  addq $8, %rax
; CHECK-NEXT:  retq
define ptr @take8() {
  ret ptr getelementptr (i8, ptr @_Znwy, i64 8)
}

; A call to the overridable default is direct, not through the import pointer.
; CHECK-LABEL: callit:
; CHECK:       jmp _Znwy
define ptr @callit(i64 %n) {
  %p = tail call ptr @_Znwy(i64 %n)
  ret ptr %p
}

; CHECK-LABEL: takelocal:
; CHECK:       leaq local(%rip), %rax
; CHECK-NEXT:  retq
define ptr @takelocal() {
  ret ptr @local
}

define ptr @_Znwy(i64 %n) #0 {
  ret ptr null
}

define dso_local ptr @local() {
  ret ptr null
}

attributes #0 = { "overridable-default" }

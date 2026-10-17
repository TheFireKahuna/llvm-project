; RUN: llc < %s -mtriple=aarch64-unknown-windows-itanium | FileCheck %s

; A function definition carrying "overridable-default" is an overridable
; default: its address is the program's one replacement, reached through its
; import pointer in every image, so an address-take, with or without an
; addend, is a load of the import pointer, while a call to it stays direct. A
; strong definition, a replacement, has a section of its own, so that its own
; unit's branches to it keep their relocations, which the link of a DLL whose
; replacement the program may not select redirects to the program's choice. A
; definition without the attribute keeps its direct address.

; CHECK-LABEL: take:
; CHECK:       adrp x0, __imp__Znwy
; CHECK-NEXT:  ldr x0, [x0, :lo12:__imp__Znwy]
; CHECK-NEXT:  ret
define ptr @take() {
  ret ptr @_Znwy
}

; CHECK-LABEL: take8:
; CHECK:       adrp x8, __imp__Znwy
; CHECK-NEXT:  ldr x8, [x8, :lo12:__imp__Znwy]
; CHECK-NEXT:  add x0, x8, #8
; CHECK-NEXT:  ret
define ptr @take8() {
  ret ptr getelementptr (i8, ptr @_Znwy, i64 8)
}

; A call to the overridable default is direct, not through the import pointer.
; CHECK-LABEL: callit:
; CHECK:       b _Znwy
define ptr @callit(i64 %n) {
  %p = tail call ptr @_Znwy(i64 %n)
  ret ptr %p
}

; CHECK-LABEL: takelocal:
; CHECK:       adrp x0, local
; CHECK-NEXT:  add x0, x0, :lo12:local
; CHECK-NEXT:  ret
define ptr @takelocal() {
  ret ptr @local
}

; CHECK:      .section .text,"xr",one_only,_Znwy,unique,0
; CHECK-NEXT: .globl _Znwy
define ptr @_Znwy(i64 %n) #0 {
  ret ptr null
}

define dso_local ptr @local() {
  ret ptr null
}

attributes #0 = { "overridable-default" }

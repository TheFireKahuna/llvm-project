; RUN: llc < %s -mtriple=x86_64-unknown-windows-itanium | FileCheck %s

; A function definition carrying "overridable-default" is an overridable
; default: its address is the program's one replacement, reached through its
; import pointer in every image, so an address-take, with or without an
; addend, is a load of the import pointer. Under -fno-plt, a call to a strong
; definition, a replacement, goes through the import pointer too, which the
; linker makes direct in the executable and keeps in a DLL whose replacement
; the program may not select; a call to a weak definition, the runtime's own
; default, stays direct. A replacement has a section of its own, as its
; branches keep their relocations. A definition without the attribute keeps its
; direct address.

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

; CHECK-LABEL: callit:
; CHECK:       callq *__imp__Znwy(%rip)
define ptr @callit(i64 %n) {
  %p = call ptr @_Znwy(i64 %n)
  ret ptr %p
}

; CHECK-LABEL: tailcallit:
; CHECK:       rex64 jmpq *__imp__Znwy(%rip)
define ptr @tailcallit(i64 %n) {
  %p = tail call ptr @_Znwy(i64 %n)
  ret ptr %p
}

; CHECK-LABEL: callweak:
; CHECK:       jmp _Znay
define ptr @callweak(i64 %n) {
  %p = tail call ptr @_Znay(i64 %n)
  ret ptr %p
}

; CHECK-LABEL: takelocal:
; CHECK:       leaq local(%rip), %rax
; CHECK-NEXT:  retq
define ptr @takelocal() {
  ret ptr @local
}

; CHECK:      .section .text,"xr",one_only,_Znwy,unique,0
; CHECK-NEXT: .globl _Znwy
define ptr @_Znwy(i64 %n) #0 {
  ret ptr null
}

; CHECK-NOT:  one_only,_Znay
define weak ptr @_Znay(i64 %n) #0 {
  ret ptr null
}

define dso_local ptr @local() {
  ret ptr null
}

attributes #0 = { "overridable-default" }

!llvm.module.flags = !{!0}
!0 = !{i32 7, !"RtLibUseGOT", i32 1}

; RUN: llc < %s -mtriple=x86_64-unknown-windows-itanium | FileCheck %s
; RUN: llc < %s -mtriple=x86_64-pc-windows-ntposix | FileCheck %s
; RUN: llc < %s -mtriple=x86_64-pc-windows-msvc | \
; RUN:   FileCheck %s --check-prefix=NONE

; A direct tail call that ends its function is described to the linker, which
; may leave the jump out where it places the callee right after the function.
; One that other code follows in its function is not.

; NONE-NOT: .linktailjump

declare i32 @callee(i32)

; CHECK-LABEL: last:
; CHECK:       .linktailjump
; CHECK-NEXT:  jmp callee
define i32 @last(i32 %x) {
  %r = tail call i32 @callee(i32 %x)
  ret i32 %r
}

; CHECK-LABEL: notlast:
; CHECK-NOT:   .linktailjump
; CHECK:       retq
define i32 @notlast(i32 %x) {
  %c = icmp eq i32 %x, 0
  br i1 %c, label %call, label %ret
call:
  %r = tail call i32 @callee(i32 %x)
  ret i32 %r
ret:
  ret i32 1
}

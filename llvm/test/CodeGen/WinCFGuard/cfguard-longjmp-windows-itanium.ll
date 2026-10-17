; RUN: llc < %s -mtriple=x86_64-unknown-windows-itanium | FileCheck %s
; RUN: llc < %s -mtriple=x86_64-pc-windows-ntposix | FileCheck %s
; RUN: %if aarch64-registered-target %{ llc < %s -mtriple=aarch64-unknown-windows-itanium | FileCheck %s %}

; Code generation lists the return sites of setjmp in .gljmp$y for Windows
; Itanium and NT-POSIX as it does for MSVC.

@buf = internal global [32 x i64] zeroinitializer, align 16
@res = internal global i32 0

declare i32 @_setjmp(ptr) returns_twice

define i32 @main() {
; CHECK-LABEL: main:
; CHECK:       {{callq|bl}} _setjmp
; CHECK-NEXT:  $cfgsj_main0:
  %r = call i32 @_setjmp(ptr @buf)
  store volatile i32 %r, ptr @res
  ret i32 0
}

; CHECK:      .section .gljmp$y,"dr"
; CHECK-NEXT: .symidx $cfgsj_main0

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"cfguard", i32 2}

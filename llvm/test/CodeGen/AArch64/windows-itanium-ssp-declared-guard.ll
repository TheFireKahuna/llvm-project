; RUN: llc -mtriple=aarch64-unknown-windows-itanium < %s | FileCheck %s --check-prefix=WI
; RUN: llc -mtriple=aarch64-pc-windows-ntposix < %s | FileCheck %s --check-prefix=NTPOSIX

; Every Windows Itanium and NT-POSIX image defines its own stack protector
; guard, so a protected function reads it directly even where the module
; declares it without dso_local, rather than through an import pointer.

@__security_cookie = external global ptr
@__stack_chk_guard = external global ptr

declare void @use(ptr)

define void @f() sspstrong {
; WI-LABEL: f:
; WI:       adrp x{{[0-9]+}}, __security_cookie
; WI:       bl use
; WI:       adrp x{{[0-9]+}}, __security_cookie
; WI:       bl __security_check_cookie
; NTPOSIX-LABEL: f:
; NTPOSIX:       adrp x{{[0-9]+}}, __stack_chk_guard
; NTPOSIX:       bl use
; NTPOSIX:       adrp x{{[0-9]+}}, __stack_chk_guard
; NTPOSIX:       bl __stack_chk_fail
  %a = alloca [16 x i8]
  call void @use(ptr %a)
  ret void
}

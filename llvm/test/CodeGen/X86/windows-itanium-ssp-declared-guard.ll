; RUN: llc -mtriple=x86_64-unknown-windows-itanium < %s | FileCheck %s --check-prefix=WI
; RUN: llc -mtriple=x86_64-pc-windows-ntposix < %s | FileCheck %s --check-prefix=NTPOSIX

; Every Windows Itanium and NT-POSIX image defines its own stack protector
; guard, so a protected function reads it directly even where the module
; declares it without dso_local, as the C runtime's own sources and the SDK's
; headers do, rather than through an import pointer.

@__security_cookie = external global ptr
@__stack_chk_guard = external global ptr

declare void @use(ptr)

define void @f() sspstrong {
; WI-LABEL: f:
; WI:       movq __security_cookie(%rip)
; WI:       callq use
; WI:       movq __security_cookie(%rip)
; WI:       callq __security_check_cookie
; NTPOSIX-LABEL: f:
; NTPOSIX:       movq __stack_chk_guard(%rip)
; NTPOSIX:       callq use
; NTPOSIX:       movq __stack_chk_guard(%rip)
; NTPOSIX:       callq __stack_chk_fail
  %a = alloca [16 x i8]
  call void @use(ptr %a)
  ret void
}

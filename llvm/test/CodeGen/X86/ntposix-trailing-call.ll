; RUN: llc -verify-machineinstrs -mtriple=x86_64-pc-windows-ntposix < %s | \
; RUN:   FileCheck %s

; NT-POSIX uses the System V convention but emits Windows x64 unwind data, so
; the Windows x64 unwinder's rules still apply: a call that ends a function
; with unwind info is followed by an int3.

declare void @abort() noreturn

define void @trailing() uwtable {
; CHECK-LABEL: trailing:
; CHECK:       .seh_endprologue
; CHECK-NEXT:  callq abort
; CHECK-NEXT:  int3
; CHECK-NEXT:  .seh_endproc
  call void @abort()
  unreachable
}

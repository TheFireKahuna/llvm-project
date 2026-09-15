; RUN: llc -O2 -verify-machineinstrs -mtriple=aarch64-pc-windows-ntposix < %s | FileCheck %s --check-prefix=NTPOSIX
; RUN: llc -O0 -verify-machineinstrs -mtriple=aarch64-pc-windows-ntposix < %s | FileCheck %s --check-prefix=NTPOSIX
; RUN: llc -O0 -verify-machineinstrs -mtriple=aarch64-pc-windows-ntposix -fast-isel < %s | FileCheck %s --check-prefix=NTPOSIX
; RUN: llc -O0 -verify-machineinstrs -mtriple=aarch64-pc-windows-ntposix -global-isel=false -fast-isel=false < %s | FileCheck %s --check-prefix=NTPOSIX
; RUN: llc -O2 -verify-machineinstrs -mtriple=aarch64-unknown-linux-gnu < %s | FileCheck %s --check-prefix=AAPCS

; On NT-POSIX the second return of a returns_twice call lands in a block that
; preserves no register, so a value live across the call crosses through the
; frame; the call itself keeps its normal mask, so the function saves no
; callee-saved register its own code does not use. The same code elsewhere
; keeps the value in a callee-saved register.

declare i32 @setjmp(ptr) returns_twice
declare void @use(i32)

define void @keep(i32 %v, ptr %buf) {
; NTPOSIX-LABEL: keep:
; NTPOSIX-NOT: x19
; NTPOSIX-NOT: d8
; NTPOSIX: str w{{[0-9]+}}, [[SLOT:\[sp, #[0-9]+\]]]
; NTPOSIX: bl setjmp
; NTPOSIX-NOT: w{{19|2[0-8]}}
; NTPOSIX: ldr w0, [[SLOT]]
; NTPOSIX: bl use
;
; AAPCS-LABEL: keep:
; AAPCS: mov w19, w0
; AAPCS: bl setjmp
; AAPCS-NEXT: mov w0, w19
; AAPCS-NEXT: bl use
  %rc = call i32 @setjmp(ptr %buf)
  call void @use(i32 %v)
  ret void
}

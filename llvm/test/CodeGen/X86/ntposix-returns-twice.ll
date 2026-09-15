; RUN: llc -O2 -verify-machineinstrs -mtriple=x86_64-pc-windows-ntposix < %s | FileCheck %s --check-prefix=NTPOSIX
; RUN: llc -O0 -verify-machineinstrs -mtriple=x86_64-pc-windows-ntposix < %s | FileCheck %s --check-prefix=NTPOSIX
; RUN: llc -O0 -verify-machineinstrs -mtriple=x86_64-pc-windows-ntposix -fast-isel=false < %s | FileCheck %s --check-prefix=NTPOSIX
; RUN: llc -O2 -verify-machineinstrs -mtriple=x86_64-unknown-linux-gnu < %s | FileCheck %s --check-prefix=SYSV

; On NT-POSIX the second return of a returns_twice call lands in a block that
; preserves no register, so a value live across the call crosses through the
; frame; the call itself keeps its normal mask, so the function saves no
; callee-saved register its own code does not use. The same SysV code
; elsewhere keeps the value in a callee-saved register.

declare i32 @setjmp(ptr) returns_twice
declare void @use(i32)

define void @keep(i32 %v, ptr %buf) {
; NTPOSIX-LABEL: keep:
; NTPOSIX-NOT: pushq %rbx
; NTPOSIX-NOT: pushq %r1{{[2-5]}}
; NTPOSIX: movl %e{{[a-z]+}}, [[SLOT:[0-9]*\(%rsp\)]]
; NTPOSIX: callq setjmp
; NTPOSIX-NOT: %e{{[bs]}}x
; NTPOSIX-NOT: %r1{{[2-5]}}d
; NTPOSIX: movl [[SLOT]], %edi
; NTPOSIX: callq use
;
; SYSV-LABEL: keep:
; SYSV: movl %edi, %ebx
; SYSV: callq setjmp
; SYSV-NEXT: movl %ebx, %edi
; SYSV-NEXT: callq use
  %rc = call i32 @setjmp(ptr %buf)
  call void @use(i32 %v)
  ret void
}

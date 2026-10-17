; RUN: llc -O2 -verify-machineinstrs -mtriple=x86_64-pc-windows-ntposix < %s | FileCheck %s --check-prefix=NTPOSIX
; RUN: llc -O0 -verify-machineinstrs -mtriple=x86_64-pc-windows-ntposix < %s | FileCheck %s --check-prefix=NTPOSIX
; RUN: llc -O0 -verify-machineinstrs -mtriple=x86_64-pc-windows-ntposix -fast-isel=false < %s | FileCheck %s --check-prefix=NTPOSIX
; RUN: llc -O2 -verify-machineinstrs -mtriple=x86_64-unknown-linux-gnu < %s | FileCheck %s --check-prefix=SYSV

; A returns_twice callee carrying "returns-twice-landing" has its second
; return land in a block that preserves no register, so a value live across
; the call crosses through the frame; the call itself keeps its normal mask,
; so the function saves no callee-saved register its own code does not use.
; Without the attribute the value stays in a callee-saved register.

declare i32 @setjmp(ptr) #0
declare i32 @setjmp_plain(ptr) returns_twice
declare void @use(i32)

define void @keep(i32 %v, ptr %buf) {
; NTPOSIX-LABEL: keep:
; NTPOSIX-NOT: pushq %rbx
; NTPOSIX-NOT: pushq %r1{{[2-5]}}
; NTPOSIX: movl %e{{[a-z]+}}, [[SLOT:[0-9]*\(%rsp\)]]
; NTPOSIX: callq setjmp
; NTPOSIX-NOT: %e{{[bs]}}x
; NTPOSIX-NOT: %r1{{[2-5]}}d
; NTPOSIX: movl [[SLOT]], %ecx
; NTPOSIX: callq use
;
; SYSV-LABEL: keep:
; SYSV: movl %edi, [[SLOT:[0-9]*\(%rsp\)]]
; SYSV: callq setjmp
; SYSV: movl [[SLOT]], %edi
; SYSV: callq use
  %rc = call i32 @setjmp(ptr %buf)
  call void @use(i32 %v)
  ret void
}

define void @plain(i32 %v, ptr %buf) {
; SYSV-LABEL: plain:
; SYSV: movl %edi, %ebx
; SYSV: callq setjmp_plain
; SYSV-NEXT: movl %ebx, %edi
; SYSV-NEXT: callq use
  %rc = call i32 @setjmp_plain(ptr %buf)
  call void @use(i32 %v)
  ret void
}

attributes #0 = { returns_twice "returns-twice-landing" }

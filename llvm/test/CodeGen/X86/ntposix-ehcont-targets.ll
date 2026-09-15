; RUN: llc -O2 -verify-machineinstrs -mtriple=x86_64-pc-windows-ntposix < %s | FileCheck %s --check-prefix=NTPOSIX
; RUN: llc -O0 -verify-machineinstrs -mtriple=x86_64-pc-windows-ntposix < %s | FileCheck %s --check-prefix=NTPOSIX
; RUN: llc -O0 -verify-machineinstrs -mtriple=x86_64-pc-windows-ntposix -fast-isel=false < %s | FileCheck %s --check-prefix=NTPOSIX
; RUN: llc -O2 -verify-machineinstrs -mtriple=x86_64-pc-windows-msvc < %s | FileCheck %s --check-prefix=MSVC

; On NT-POSIX two continuations besides a landing pad are targets the kernel
; validates against the EH continuation table: the second return of a
; returns_twice call, and an inline-asm indirect target, which is where a
; fault handler resumes the asm's own frame. Both are recorded there.

declare i32 @setjmp(ptr) returns_twice
declare void @use(i32)

define void @jump(ptr %buf) {
; NTPOSIX-LABEL: jump:
; NTPOSIX: callq setjmp
; NTPOSIX: $ehgcr_0_{{[0-9]+}}:
  %rc = call i32 @setjmp(ptr %buf)
  call void @use(i32 %rc)
  ret void
}

define i64 @probe(ptr %addr) {
; NTPOSIX-LABEL: probe:
; NTPOSIX: movq (%{{[a-z0-9]+}}), %{{[a-z0-9]+}}
; NTPOSIX: $ehgcr_1_{{[0-9]+}}:
  %v = callbr i64 asm "movq ($1), $0", "=r,r,!i"(ptr %addr) to label %ok [label %fault]
ok:
  ret i64 %v
fault:
  ret i64 -1
}

; NTPOSIX: .section .gehcont$y
; NTPOSIX-NEXT: .symidx $ehgcr_0_{{[0-9]+}}
; NTPOSIX-NEXT: .symidx $ehgcr_1_{{[0-9]+}}
; MSVC-NOT: .gehcont$y
; MSVC-NOT: $ehgcr

!llvm.module.flags = !{!0}
!0 = !{i32 1, !"ehcontguard", i32 1}

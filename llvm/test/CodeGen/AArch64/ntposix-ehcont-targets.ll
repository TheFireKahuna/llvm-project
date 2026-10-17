; RUN: llc -O2 -verify-machineinstrs -mtriple=aarch64-pc-windows-ntposix < %s | FileCheck %s --check-prefix=NTPOSIX
; RUN: llc -O0 -verify-machineinstrs -mtriple=aarch64-pc-windows-ntposix < %s | FileCheck %s --check-prefix=NTPOSIX
; RUN: llc -O0 -verify-machineinstrs -mtriple=aarch64-pc-windows-ntposix -fast-isel < %s | FileCheck %s --check-prefix=NTPOSIX
; RUN: llc -O0 -verify-machineinstrs -mtriple=aarch64-pc-windows-ntposix -global-isel=false -fast-isel=false < %s | FileCheck %s --check-prefix=NTPOSIX
; RUN: llc -O2 -verify-machineinstrs -mtriple=aarch64-pc-windows-msvc < %s | FileCheck %s --check-prefix=MSVC

; The second return of a call to a returns_twice function carrying the
; "returns-twice-landing" attribute is a continuation the kernel validates
; against the EH continuation table, so it is recorded there beside the
; landing pads, on every Windows triple under ehcontguard.

declare i32 @setjmp(ptr) #0
declare void @use(i32)

define void @jump(ptr %buf) {
; NTPOSIX-LABEL: jump:
; NTPOSIX: bl setjmp
; NTPOSIX: $ehgcr_0_{{[0-9]+}}:
  %rc = call i32 @setjmp(ptr %buf)
  call void @use(i32 %rc)
  ret void
}

; NTPOSIX: .section .gehcont$y
; NTPOSIX-NEXT: .symidx $ehgcr_0_{{[0-9]+}}
; MSVC: .section .gehcont$y
; MSVC-NEXT: .symidx $ehgcr_0_{{[0-9]+}}

attributes #0 = { returns_twice "returns-twice-landing" }

!llvm.module.flags = !{!0}
!0 = !{i32 1, !"ehcontguard", i32 1}

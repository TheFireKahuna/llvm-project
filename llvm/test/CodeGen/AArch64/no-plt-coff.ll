; RUN: llc < %s -mtriple=aarch64-pc-windows-msvc | FileCheck %s
; RUN: llc < %s -mtriple=aarch64-w64-windows-gnu | FileCheck %s
; RUN: llc < %s -mtriple=aarch64-pc-windows-msvc -global-isel \
; RUN:   -global-isel-abort=2 2>/dev/null | FileCheck %s
; RUN: llc < %s -mtriple=aarch64-pc-windows-msvc -O0 | FileCheck %s
; RUN: llc < %s -mtriple=aarch64-unknown-windows-itanium -global-isel \
; RUN:   | FileCheck %s --check-prefixes=CHECK,WI

; With RtLibUseGOT (-fno-plt) a runtime library call is loaded through the
; GOT, which on COFF is the function's import pointer, not the function's
; first bytes.

define void @copy(ptr %d, ptr %s, i64 %n) {
; CHECK-LABEL: copy:
; CHECK:       adrp [[REG:x[0-9]+]], __imp_memcpy
; CHECK-NEXT:  ldr [[REG]], [[[REG]], :{{(fused_)?}}lo12:__imp_memcpy]
; CHECK:       {{blr|br}} [[REG]]
  call void @llvm.memcpy.p0.p0.i64(ptr %d, ptr %s, i64 %n, i1 false)
  ret void
}

; The stack protector's call exists before the combiners run.
define void @protected() sspstrong {
; WI-LABEL:    protected:
; WI:          adrp [[REG:x[0-9]+]], __imp___stack_chk_fail
; WI-NEXT:     ldr [[REG]], [[[REG]], :fused_lo12:__imp___stack_chk_fail]
; WI:          blr [[REG]]
  %a = alloca [16 x i8]
  call void @use(ptr %a)
  ret void
}

declare void @use(ptr)
declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)

!llvm.module.flags = !{!0}
!0 = !{i32 7, !"RtLibUseGOT", i32 1}

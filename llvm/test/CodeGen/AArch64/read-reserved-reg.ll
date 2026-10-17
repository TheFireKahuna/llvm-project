; RUN: llc < %s -mtriple=aarch64-pc-windows-msvc | FileCheck %s
; RUN: llc < %s -mtriple=aarch64-unknown-windows-itanium | FileCheck %s
; RUN: llc < %s -mtriple=aarch64-linux-gnu -mattr=+reserve-x18 | FileCheck %s

; A reserved register, such as x18 on Windows, which holds the TEB, is read by
; a copy that folds into its users, so the load addresses x18 directly.

; CHECK-LABEL: peb:
; CHECK-NOT:   mov
; CHECK:       ldr x8, [x18, #96]
; CHECK-NEXT:  ldr x0, [x8, #16]
; CHECK-NEXT:  ret
define ptr @peb() {
  %teb = call i64 @llvm.read_register.i64(metadata !0)
  %p = inttoptr i64 %teb to ptr
  %pebp = getelementptr i8, ptr %p, i64 96
  %peb = load ptr, ptr %pebp, align 8
  %basep = getelementptr i8, ptr %peb, i64 16
  %base = load ptr, ptr %basep, align 8
  ret ptr %base
}

declare i64 @llvm.read_register.i64(metadata)

!0 = !{!"x18"}

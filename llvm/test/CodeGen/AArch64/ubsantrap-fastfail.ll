; RUN: llc -mtriple=aarch64-unknown-windows-itanium < %s | FileCheck %s
; RUN: llc -mtriple=aarch64-unknown-windows-ntposix < %s | FileCheck %s
; RUN: llc -mtriple=aarch64-unknown-windows-itanium -O0 -global-isel < %s | \
; RUN:   FileCheck %s
; RUN: llc -mtriple=aarch64-pc-windows-msvc < %s | FileCheck --check-prefix=MSVC %s

; On Windows Itanium and NT-POSIX, a sanitizer trap whose kind is 64 or more
; fails fast with the kind as its code; other kinds stay brk.

; CHECK-LABEL: f:
; CHECK:       mov w0, #64
; CHECK-NEXT:  brk #0xf003
; CHECK:       mov w0, #65
; CHECK-NEXT:  brk #0xf003
; CHECK:       brk #0x5502
; MSVC:        brk #0x5540
; MSVC:        brk #0x5541
define void @f(i1 %c, i1 %d) {
  br i1 %c, label %t1, label %n
t1:
  call void @llvm.ubsantrap(i8 64) nomerge
  unreachable
n:
  br i1 %d, label %t2, label %e
t2:
  call void @llvm.ubsantrap(i8 65) nomerge
  unreachable
e:
  call void @llvm.ubsantrap(i8 2) nomerge
  unreachable
}

declare void @llvm.ubsantrap(i8)

; RUN: llc -mtriple=x86_64-unknown-windows-itanium < %s | FileCheck %s
; RUN: llc -mtriple=x86_64-pc-windows-ntposix < %s | FileCheck %s
; RUN: %if aarch64-registered-target %{ llc -mtriple=aarch64-unknown-windows-itanium < %s | FileCheck %s %}
; RUN: llc -mtriple=x86_64-scei-windows-itanium < %s | FileCheck %s --check-prefix=SCEI

; Windows Itanium and NT-POSIX put each global in a section of its own by
; default, since the linker takes a data object's extent from its section.

@a = global i32 1
@b = global i32 2

; CHECK: .section .data,"dw",one_only,a
; CHECK: a:
; CHECK: .section .data,"dw",one_only,b
; CHECK: b:
; SCEI:      .data
; SCEI-NOT:  .section
; SCEI:      b:

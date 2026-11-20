;; On Windows Itanium and NT-POSIX a local global in llvm.used, which /INCLUDE
;; cannot name, takes a section of its own without COMDAT, which the linker
;; keeps. An external one keeps its COMDAT and /INCLUDE, a global in
;; llvm.compiler.used only its COMDAT, and other COFF targets are unchanged.

; DEFINE: %{llc} = llc -data-sections -function-sections < %s
; RUN: %{llc} -mtriple=x86_64-unknown-windows-itanium | \
; RUN:   FileCheck %s --check-prefixes=CHECK,RETAIN
; RUN: %{llc} -mtriple=x86_64-pc-windows-ntposix | \
; RUN:   FileCheck %s --check-prefixes=CHECK,RETAIN
; RUN: %if aarch64-registered-target %{ %{llc} -mtriple=aarch64-unknown-windows-itanium | \
; RUN:   FileCheck %s --check-prefixes=CHECK,RETAIN %}
; RUN: %if aarch64-registered-target %{ %{llc} -mtriple=aarch64-pc-windows-ntposix | \
; RUN:   FileCheck %s --check-prefixes=CHECK,RETAIN %}
; RUN: %{llc} -mtriple=x86_64-pc-windows-msvc | \
; RUN:   FileCheck %s --check-prefixes=CHECK,MSVC

@ext = global i32 1
@loc = internal global i32 2
@cu = internal global i32 3

define internal void @lf() {
  ret void
}

; RETAIN:      .section .text,"xr",unique,[[#]]
; MSVC:        .section .text,"xr",one_only,lf,unique,[[#]]
; CHECK-NEXT:  .p2align
; CHECK-NEXT:  lf:

; CHECK:       .section .data,"dw",one_only,ext,unique,[[#]]
; CHECK:       ext:

; RETAIN:      .section .data,"dw",unique,[[#]]
; MSVC:        .section .data,"dw",one_only,loc,unique,[[#]]
; CHECK-NEXT:  .p2align
; CHECK-NEXT:  loc:

; CHECK:       .section .data,"dw",one_only,cu,unique,[[#]]
; CHECK:       cu:

; CHECK:       .ascii " /INCLUDE:ext"

@llvm.used = appending global [3 x ptr] [ptr @ext, ptr @loc, ptr @lf], section "llvm.metadata"
@llvm.compiler.used = appending global [1 x ptr] [ptr @cu], section "llvm.metadata"

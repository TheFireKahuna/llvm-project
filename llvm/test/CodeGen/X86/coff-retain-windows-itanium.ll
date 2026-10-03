; On Windows Itanium and NT-POSIX a global in llvm.used is kept through the
; link, as on ELF: an external one through /INCLUDE, and a local one, which
; /INCLUDE cannot name, in the shared section rather than a COMDAT the linker
; could discard. llvm.compiler.used keeps a global from the compiler only.
;
; RUN: llc -mtriple=x86_64-unknown-windows-itanium -data-sections -function-sections < %s | FileCheck %s --check-prefixes=CHECK,RETAIN
; RUN: llc -mtriple=x86_64-pc-windows-ntposix -data-sections -function-sections < %s | FileCheck %s --check-prefixes=CHECK,RETAIN
; RUN: llc -mtriple=x86_64-pc-windows-msvc -data-sections -function-sections < %s | FileCheck %s --check-prefixes=CHECK,MSVC
; RUN: llc -mtriple=x86_64-w64-windows-gnu -data-sections -function-sections < %s | FileCheck %s --check-prefixes=CHECK,MINGW

@ext = global i32 1
@loc = internal global i32 2
@cu = internal global i32 3

define internal void @lf() {
  ret void
}

; RETAIN:      .text
; RETAIN-NEXT: .p2align
; RETAIN-NEXT: lf:
; MSVC:        .section .text,"xr",one_only,lf,unique,{{[0-9]+}}
; MSVC-NEXT:   .p2align
; MSVC-NEXT:   lf:

; CHECK:       .section .data{{(\$ext)?}},"dw",one_only,ext,
; CHECK:       ext:

; RETAIN:      .data
; RETAIN-NEXT: .p2align
; RETAIN-NEXT: loc:
; MSVC:        .section .data,"dw",one_only,loc,unique,{{[0-9]+}}
; MSVC-NEXT:   .p2align
; MSVC-NEXT:   loc:

; CHECK:       .section .data{{(\$cu)?}},"dw",one_only,cu,
; CHECK:       cu:

; RETAIN:      .section .drectve,"yni"
; RETAIN-NEXT: .ascii " /INCLUDE:ext"
; MSVC:        .ascii " /INCLUDE:ext"
; MINGW-NOT:   /INCLUDE:

@llvm.used = appending global [3 x ptr] [ptr @ext, ptr @loc, ptr @lf], section "llvm.metadata"
@llvm.compiler.used = appending global [1 x ptr] [ptr @cu], section "llvm.metadata"

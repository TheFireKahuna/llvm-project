; REQUIRES: x86, aarch64

;; A declaration that names its alternate is a weak external of it, which LTO
;; does not link: the link binds it to the alternate wherever that is and
;; keeps the alternate from internalization. A destructor whose base
;; destructor another unit defines is such a declaration.

; RUN: rm -rf %t && split-file %s %t && cd %t
; RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium %S/Inputs/fill-entry.s -o fill-x86_64.obj
; RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium %S/Inputs/fill-entry.s -o fill-aarch64.obj

; DEFINE: %{t} = x86_64
; DEFINE: %{run} = opt -mtriple=%{t}-windows-itanium main.ll -o main.bc && \
; DEFINE:   opt -mtriple=%{t}-windows-itanium base.ll -o base.bc && \
; DEFINE:   lld-link -dll -noentry fill-%{t}.obj main.bc base.bc -out:out.dll && \
; DEFINE:   llvm-readobj --coff-exports out.dll | FileCheck %s

; RUN: %{run}
; REDEFINE: %{t} = aarch64
; RUN: %{run}

; CHECK:      Name: c{{$}}

;--- main.ll
declare extern_weak dllexport i32 @c() "weak-alternate"="b"

;--- base.ll
define i32 @b() {
  ret i32 6
}

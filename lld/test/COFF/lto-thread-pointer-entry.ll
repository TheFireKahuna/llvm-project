; REQUIRES: x86, aarch64

;; The code LTO generates for a function X$tp defines its plain entry X, which
;; the bitcode does not list: the link takes X as defined, keeps X$tp where an
;; object that passes no thread pointer references X, and exports X with an
;; exported X$tp. The subsystem is inferred from main.

; RUN: rm -rf %t && split-file %s %t && cd %t
; RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium %S/Inputs/fill-entry.s -o fill-x86_64.obj
; RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium %S/Inputs/fill-entry.s -o fill-aarch64.obj

; DEFINE: %{t} = x86_64
; DEFINE: %{run} = opt -mtriple=%{t}-windows-itanium main.ll -o main.bc && \
; DEFINE:   llvm-mc -filetype=obj -triple=%{t}-windows-itanium start-%{t}.s -o start.obj && \
; DEFINE:   lld-link -entry:start fill-%{t}.obj main.bc start.obj -out:main.exe && \
; DEFINE:   llvm-readobj --file-headers --coff-exports main.exe | FileCheck %s

; RUN: %{run}
; REDEFINE: %{t} = aarch64
; RUN: %{run}

; CHECK: Subsystem: IMAGE_SUBSYSTEM_WINDOWS_CUI
; CHECK: Name: f{{$}}
; CHECK: Name: f$tp

;--- main.ll
define i32 @main() "thread-pointer-argument" "thread-pointer-unused" {
  ret i32 0
}

define dllexport void @f() "thread-pointer-argument" "thread-pointer-unused" {
  ret void
}

;--- start-x86_64.s
  .globl start
start:
  jmpq *__imp_main(%rip)

;--- start-aarch64.s
  .globl start
start:
  adrp x16, __imp_main
  ldr x16, [x16, :lo12:__imp_main]
  br x16

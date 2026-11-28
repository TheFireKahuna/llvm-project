; REQUIRES: x86

;; A member that only the code LTO generates calls is loaded after LTO, and
;; the image exports what its directives export.

; RUN: rm -rf %t && split-file %s %t && cd %t
; RUN: llvm-as main.ll -o main.obj
; RUN: llvm-mc -triple=x86_64-pc-windows-msvc -filetype=obj use.s -o use.obj
; RUN: llvm-mc -triple=x86_64-pc-windows-msvc -filetype=obj chkstk.s -o chkstk.obj
; RUN: llvm-ar rcs chkstk.lib chkstk.obj
; RUN: lld-link -out:main.exe -entry:main -subsystem:console main.obj use.obj chkstk.lib
; RUN: llvm-readobj --coff-exports main.exe | FileCheck %s

; CHECK: Name: __chkstk

;--- main.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

define void @main() {
  %array = alloca [8192 x i8]
  call void @use(ptr %array)
  ret void
}

declare void @use(ptr)

;--- use.s
  .text
  .globl use
use:
  ret

;--- chkstk.s
  .text
  .globl __chkstk
__chkstk:
  ret
  .section .drectve,"yni"
  .ascii " /export:__chkstk"

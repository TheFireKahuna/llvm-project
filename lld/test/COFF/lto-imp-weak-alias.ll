; REQUIRES: x86
;; An __imp_ reference to a weak external whose alias is defined is resolved
;; with a local import, so the check for unresolvable symbols before LTO
;; accepts it, as the link without LTO does.

; RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
; RUN: llvm-as main.ll -o main.bc
; RUN: llc main.ll -filetype=obj -o main.obj
; RUN: llvm-mc -triple=x86_64-windows-msvc -filetype=obj weak.s -o weak.obj
; RUN: lld-link -dll -noentry -export:use -out:obj.dll main.obj weak.obj
; RUN: lld-link -dll -noentry -export:use -out:lto.dll main.bc weak.obj

;--- main.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

declare dllimport void @foo()

define void @use() {
  call void @foo()
  ret void
}

;--- weak.s
.text
.weak foo
foo:
  ret

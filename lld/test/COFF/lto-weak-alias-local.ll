; REQUIRES: x86
;; A weak alias of a function local to its module, such as the init function
;; of an inline thread-local variable, is a weak definition of the module: the
;; object the module compiles to gives the alias the local function as its
;; default. Two modules may each define it.

; RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
; RUN: llvm-as a.ll -o a.obj
; RUN: llvm-as b.ll -o b.obj
; RUN: lld-link -entry:main -subsystem:console a.obj b.obj -out:main.exe
; RUN: llvm-objdump -d main.exe | FileCheck %s

; CHECK:      <.text>:
; CHECK-NEXT: retq

;--- a.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

@init = linkonce_odr alias void (), ptr @init.a

define internal void @init.a() {
  ret void
}

define void @main() {
  call void @init()
  call void @other()
  ret void
}

declare void @other()

;--- b.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

@init = linkonce_odr alias void (), ptr @init.b

define internal void @init.b() {
  ret void
}

define void @other() {
  call void @init()
  ret void
}

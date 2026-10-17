; REQUIRES: x86
;; An executable whose replacement of an overridable default is in a bitcode
;; file replaces it as one in an object does: the check that it replaces one
;; runs before LTO, and the executable gets its override table.

; RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
; RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium l.s -o l.obj
; RUN: llvm-as main.ll -o main.obj
; RUN: lld-link -import-slots -dll -noentry l.obj -out:l.dll -implib:l.lib
; RUN: lld-link -import-slots main.obj l.lib -out:main.exe -entry:main \
; RUN:   -subsystem:console
; RUN: llvm-objdump -h main.exe | FileCheck %s
; CHECK: .ovra

;--- l.s
        .text
        .weak hook
hook:
        xorl %eax, %eax
        retq
        .section .drectve,"yni"
        .ascii " /OVERRIDABLEBYNAME:hook /EXPORT:hook"

;--- main.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-windows-itanium"

@__ovr_check = global i64 0

define i32 @hook() {
  ret i32 1
}

define i32 @main() {
  %r = call i32 @hook()
  ret i32 %r
}

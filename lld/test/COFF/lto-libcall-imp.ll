; REQUIRES: x86

;; Under -fno-plt a library call that the LTO step introduces takes the
;; import form, and no input names it before LTO. Under -import-slots, the
;; archive member that defines it is loaded afterwards, as is a member that
;; one loaded this way asks for.

; RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
; RUN: llvm-as main.ll -o main.bc
; RUN: opt -module-summary main.ll -o main.thin.bc
; RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc udivti3.s -o udivti3.obj
; RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc udivmodti4.s -o udivmodti4.obj
; RUN: llvm-lib udivti3.obj udivmodti4.obj -out:builtins.lib
; RUN: lld-link -import-slots -entry:main -subsystem:console -out:full.exe \
; RUN:   main.bc builtins.lib -verbose 2>&1 | FileCheck %s
; RUN: lld-link -import-slots -entry:main -subsystem:console -out:thin.exe \
; RUN:   main.thin.bc builtins.lib -verbose 2>&1 | FileCheck %s

; CHECK: Loading lazy __udivti3 from builtins.lib for __imp___udivti3
; CHECK: Loading lazy __udivmodti4 from builtins.lib for __imp___udivmodti4

;--- main.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-windows-itanium"

define i64 @main(i128 %a, i128 %b) {
  %q = udiv i128 %a, %b
  %r = trunc i128 %q to i64
  ret i64 %r
}

!llvm.module.flags = !{!0}
!0 = !{i32 7, !"RtLibUseGOT", i32 1}

;--- udivti3.s
.text
.globl __udivti3
__udivti3:
  call *__imp___udivmodti4(%rip)
  ret

;--- udivmodti4.s
.text
.globl __udivmodti4
__udivmodti4:
  ret

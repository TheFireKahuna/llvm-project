; REQUIRES: x86

; A library call the LTO step introduces takes the import form under
; -fno-plt, and nothing names it before LTO. The archive member that defines
; it is loaded afterwards, as is a member that one loaded this way needs.

; RUN: rm -rf %t.dir
; RUN: split-file %s %t.dir
; RUN: llvm-as %t.dir/main.ll -o %t.main.bc
; RUN: opt -module-summary %t.dir/main.ll -o %t.main.thin.bc
; RUN: llvm-mc -triple=x86_64-windows-msvc -filetype=obj %t.dir/udivti3.s -o %t.udivti3.obj
; RUN: llvm-mc -triple=x86_64-windows-msvc -filetype=obj %t.dir/udivmodti4.s -o %t.udivmodti4.obj
; RUN: llvm-lib -out:%t.builtins.lib %t.udivti3.obj %t.udivmodti4.obj

; RUN: lld-link -entry:main -subsystem:console -debug:symtab -out:%t1.exe %t.main.bc %t.builtins.lib -verbose 2>&1 | FileCheck %s
; RUN: llvm-objdump -d %t1.exe | FileCheck %s --check-prefix=DISASM
; RUN: lld-link -entry:main -subsystem:console -debug:symtab -out:%t2.exe %t.main.thin.bc %t.builtins.lib -verbose 2>&1 | FileCheck %s
; RUN: llvm-objdump -d %t2.exe | FileCheck %s --check-prefix=DISASM

; CHECK-DAG: Loading lazy __udivti3 from {{.*}}builtins.lib for __imp___udivti3
; CHECK-DAG: Loading lazy __udivmodti4 from {{.*}}builtins.lib for __imp___udivmodti4

;; Both calls are direct once the members are in the image.
; DISASM: <main>:
; DISASM-NOT: rip
; DISASM: addr32 callq {{.*}} <__udivti3>
; DISASM: <__udivti3>:
; DISASM-NEXT: addr32 callq {{.*}} <__udivmodti4>

;--- main.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-windows-itanium"

define i32 @main(i128 %a, i128 %b) {
  %q = udiv i128 %a, %b
  %r = trunc i128 %q to i32
  ret i32 %r
}

!llvm.module.flags = !{!0}
!0 = !{i32 7, !"RtLibUseGOT", i32 1}

;--- udivti3.s
        .text
        .globl  __udivti3
__udivti3:
        callq   *__imp___udivmodti4(%rip)
        retq

;--- udivmodti4.s
        .text
        .globl  __udivmodti4
__udivmodti4:
        retq

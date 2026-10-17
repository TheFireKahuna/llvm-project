; ThinLTO import brings the ABI requirement rows of the source module along
; with an imported function: a row of a symbol the imported body references
; names the destination's declaration of it, which already existed, and the
; row of a symbol the destination has no copy of names nothing, so that code
; generation drops it.

; RUN: opt -module-summary %s -o %t1.bc
; RUN: opt -module-summary %p/Inputs/abi-requirements-import.ll -o %t2.bc
; RUN: llvm-lto -thinlto-action=thinlink -o %t.index.bc %t1.bc %t2.bc
; RUN: llvm-lto -thinlto-action=import %t1.bc -thinlto-index=%t.index.bc -o - \
; RUN:   | llvm-dis -o - | FileCheck %s

; CHECK: @s = external global [2 x i32]
; CHECK: define available_externally i32 @f()
; CHECK: !abi.requirements = !{![[#S:]], ![[#OTHER:]]}
; CHECK-DAG: ![[#S]] = !{ptr @s, ![[#CS:]]}
; CHECK-DAG: ![[#CS]] = !{!"cS"}
; CHECK-DAG: ![[#OTHER]] = !{null, ![[#CO:]]}
; CHECK-DAG: ![[#CO]] = !{!"cO"}

target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-windows-itanium"

@s = external global [2 x i32]

define i32 @main() {
entry:
  %v = load i32, ptr @s
  %r = call i32 @f()
  %t = add i32 %v, %r
  ret i32 %t
}

declare i32 @f()

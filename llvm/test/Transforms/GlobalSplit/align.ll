; RUN: opt -S -passes=globalsplit %s | FileCheck %s

; Each piece of a split global keeps the alignment its offset had in the
; global.

target datalayout = "e-p:64:64"
target triple = "x86_64-unknown-linux-gnu"

; CHECK: @global.0 = private constant [3 x ptr] [ptr @f, ptr @f, ptr @f], align 64
; CHECK: @global.1 = private constant [1 x ptr] [ptr @f], align 8
; CHECK: @global.2 = private constant [2 x ptr] [ptr @f, ptr @f], align 32
@global = internal constant { [3 x ptr], [1 x ptr], [2 x ptr] } {
  [3 x ptr] [ptr @f, ptr @f, ptr @f],
  [1 x ptr] [ptr @f],
  [2 x ptr] [ptr @f, ptr @f]
}, align 64, !type !0, !type !1, !type !2

@vtt = constant [3 x ptr] [
  ptr getelementptr inrange(0, 24) ({ [3 x ptr], [1 x ptr], [2 x ptr] }, ptr @global, i32 0, i32 0, i32 0),
  ptr getelementptr inrange(0, 8) ({ [3 x ptr], [1 x ptr], [2 x ptr] }, ptr @global, i32 0, i32 1, i32 0),
  ptr getelementptr inrange(0, 16) ({ [3 x ptr], [1 x ptr], [2 x ptr] }, ptr @global, i32 0, i32 2, i32 0)
]

define ptr @f() {
  ret ptr null
}

define void @foo() {
  %p = call i1 @llvm.type.test(ptr null, metadata !"")
  ret void
}

declare i1 @llvm.type.test(ptr, metadata) nounwind readnone

!0 = !{i32 0, !"a"}
!1 = !{i32 24, !"b"}
!2 = !{i32 32, !"c"}

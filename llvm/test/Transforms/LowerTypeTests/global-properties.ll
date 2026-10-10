; RUN: opt -S -passes=lowertypetests %s | FileCheck %s

; The aliases that replace the globals of a combined global keep the
; properties of the globals they replace.

target datalayout = "e-p:64:64"
target triple = "x86_64-pc-windows-msvc"

; CHECK: @a = dllexport alias [2 x ptr], ptr [[G:@[0-9]+]]
; CHECK: @b = dso_local unnamed_addr alias [2 x ptr], getelementptr inbounds (i8, ptr [[G]], i64 16)
; CHECK: @c = local_unnamed_addr alias [2 x ptr], getelementptr inbounds (i8, ptr [[G]], i64 32), partition "part"
@a = dllexport constant [2 x ptr] zeroinitializer, !type !0
@b = dso_local unnamed_addr constant [2 x ptr] zeroinitializer, !type !0
@c = local_unnamed_addr constant [2 x ptr] zeroinitializer, partition "part", !type !0

!0 = !{i64 0, !"typeid"}

declare i1 @llvm.type.test(ptr, metadata)

define i1 @f(ptr %p) {
  %x = call i1 @llvm.type.test(ptr %p, metadata !"typeid")
  ret i1 %x
}

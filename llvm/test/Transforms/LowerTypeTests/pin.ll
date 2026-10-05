; RUN: opt -S -passes=lowertypetests %s | FileCheck %s

; Globals merged into a combined global keep their pins. A pin of modulus 64
; is kept by aligning the combined global; pins of modulus 4096 fix its
; address modulo 4096, are carried onto it, and their globals are placed in
; the order of the residues they want, the largest other globals that fit
; filling the gaps between them.

target datalayout = "e-p:64:64"

; A pin that is not required and cannot hold is dropped.
; CHECK: [[H:@[0-9]+]] = private constant { [2 x ptr] } zeroinitializer, align 64{{$}}
; CHECK: [[G:@[0-9]+]] = private constant {{.*}} zeroinitializer, align 64, !pin [[P2:![0-9]+]], !pin [[P1:![0-9]+]]
; CHECK: @h = alias [2 x ptr], ptr [[H]]
; CHECK: @t2 = alias [3 x ptr], getelementptr inbounds (i8, ptr [[G]], i64 8)
; CHECK: @v = alias [6 x ptr], getelementptr inbounds (i8, ptr [[G]], i64 48)
; CHECK: @a = alias [4 x ptr], getelementptr inbounds (i8, ptr [[G]], i64 128)
; CHECK: @f2 = alias [2 x ptr], getelementptr inbounds (i8, ptr [[G]], i64 160)
; CHECK: @t1 = alias [3 x ptr], getelementptr inbounds (i8, ptr [[G]], i64 328)
; CHECK: @f1 = alias [40 x ptr], getelementptr inbounds (i8, ptr [[G]], i64 352)
; CHECK: [[P2]] = !{i64 24, i64 12, i64 280, i64 1}
; CHECK: [[P1]] = !{i64 344, i64 12, i64 600, i64 1}

@t1 = constant [3 x ptr] zeroinitializer, align 8, !type !0, !pin !10
@t2 = constant [3 x ptr] zeroinitializer, align 8, !type !0, !pin !11
@a = constant [4 x ptr] zeroinitializer, align 64, !type !0
@v = constant [6 x ptr] zeroinitializer, align 8, !type !1, !pin !12
@f1 = constant [40 x ptr] zeroinitializer, align 8, !type !0
@f2 = constant [2 x ptr] zeroinitializer, align 8, !type !0

; A pin that is not required and cannot hold is dropped.
@h = constant [2 x ptr] zeroinitializer, align 64, !type !2, !pin !13

!0 = !{i64 16, !"base"}
!1 = !{i64 32, !"base"}
!2 = !{i64 0, !"other"}
!10 = !{i64 16, i64 12, i64 600, i64 1}
!11 = !{i64 16, i64 12, i64 280, i64 1}
!12 = !{i64 32, i64 6, i64 16, i64 0}
!13 = !{i64 0, i64 12, i64 8, i64 0}

declare i1 @llvm.type.test(ptr, metadata)

define i1 @f(ptr %p) {
  %x = call i1 @llvm.type.test(ptr %p, metadata !"base")
  %y = call i1 @llvm.type.test(ptr %p, metadata !"other")
  %z = and i1 %x, %y
  ret i1 %z
}

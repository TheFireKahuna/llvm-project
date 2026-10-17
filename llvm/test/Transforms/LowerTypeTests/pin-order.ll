; RUN: opt -S -passes=lowertypetests %s | FileCheck %s

; The globals that fill the gaps between globals with wide pins keep their
; order, so the members of a type identifier without such a pin stay together
; and its test needs no bit set.

target datalayout = "e-p:64:64"

; CHECK: @x1 = alias [4 x ptr], ptr [[G:@[0-9]+]]
; CHECK: @s = alias [4 x ptr], getelementptr inbounds (i8, ptr [[G]], i64 32)
; CHECK: @y1 = alias [4 x ptr], getelementptr inbounds (i8, ptr [[G]], i64 64)
; CHECK: @y2 = alias [8 x ptr], getelementptr inbounds (i8, ptr [[G]], i64 96)
; CHECK: @x2 = alias [4 x ptr], getelementptr inbounds (i8, ptr [[G]], i64 2048)

@x1 = constant [4 x ptr] zeroinitializer, align 8, !type !0, !pin !10
@x2 = constant [4 x ptr] zeroinitializer, align 8, !type !0, !pin !11
@s = constant [4 x ptr] zeroinitializer, align 8, !type !0, !type !1
@y1 = constant [4 x ptr] zeroinitializer, align 8, !type !1
@y2 = constant [8 x ptr] zeroinitializer, align 8, !type !1

!0 = !{i64 0, !"x"}
!1 = !{i64 0, !"y"}
!10 = !{i64 0, i64 12, i64 0, i64 1}
!11 = !{i64 0, i64 12, i64 2048, i64 1}

declare i1 @llvm.type.test(ptr, metadata)

; CHECK-LABEL: define i1 @fy(
; CHECK:         [[R:%[0-9]+]] = icmp ule i64 {{%[0-9]+}}, 2
; CHECK-NEXT:    ret i1 [[R]]
define i1 @fy(ptr %p) {
  %x = call i1 @llvm.type.test(ptr %p, metadata !"y")
  ret i1 %x
}

define i1 @fx(ptr %p) {
  %x = call i1 @llvm.type.test(ptr %p, metadata !"x")
  ret i1 %x
}

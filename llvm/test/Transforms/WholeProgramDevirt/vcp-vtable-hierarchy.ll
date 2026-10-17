; RUN: opt -S -passes=wholeprogramdevirt -whole-program-visibility %s | FileCheck %s

; Virtual constant propagation moves a vtable's contents into a larger global
; with constants before them; vtable_hierarchy metadata moves with them, as
; type metadata does.

target datalayout = "e-p:64:64"

; CHECK: [[VT1DATA:@[^ ]*]] = private constant { [8 x i8], [2 x ptr], [0 x i8] } {{.*}}, !type [[T8:![0-9]+]], {{.*}}!vtable_hierarchy [[H:![0-9]+]]
@vt1 = constant [2 x ptr] [ptr @vf1, ptr @vf2], !type !0, !vtable_hierarchy !1

; CHECK: [[VT2DATA:@[^ ]*]] = private constant { [8 x i8], [2 x ptr], [0 x i8] } {{.*}}, !type [[T8]]
; CHECK-NOT: !vtable_hierarchy
@vt2 = constant [2 x ptr] [ptr @vf2, ptr @vf1], !type !0

; CHECK: @vt1 = alias [2 x ptr], getelementptr inbounds (i8, ptr [[VT1DATA]], i64 8)

define i32 @vf1(ptr %this) readnone {
  ret i32 1
}

define i32 @vf2(ptr %this) readnone {
  ret i32 2
}

define i32 @call(ptr %obj) {
  %vtable = load ptr, ptr %obj
  %p = call i1 @llvm.type.test(ptr %vtable, metadata !"typeid")
  call void @llvm.assume(i1 %p)
  %fptr = load ptr, ptr %vtable
  %result = call i32 %fptr(ptr %obj)
  ret i32 %result
}

declare i1 @llvm.type.test(ptr, metadata)
declare void @llvm.assume(i1)

; CHECK: [[T8]] = !{i32 8, !"typeid"}
; CHECK-DAG: [[H]] = !{i64 8, !"1D", !"1B"}
!0 = !{i32 0, !"typeid"}
!1 = !{i64 0, !"1D", !"1B"}

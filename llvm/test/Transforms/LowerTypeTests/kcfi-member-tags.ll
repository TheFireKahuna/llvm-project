; RUN: opt -S -passes=lowertypetests %s | FileCheck %s

;; Where KCFI checks go through per-type thunks, a function type is checked by
;; membership tags instead of a jump table: no address changes, each member
;; carries its class's tag, and each type test becomes a test of the tags of
;; the type's classes.

target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-windows-itanium"

; CHECK-NOT: .cfi.jumptable
; CHECK: @table = constant [3 x ptr] [ptr @a, ptr @b, ptr @local_taken]
@table = constant [3 x ptr] [ptr @a, ptr @b, ptr @local_taken]

;; @a and @b belong to typeid1 only: one class.
; CHECK: define void @a() !type !{{[0-9]+}} !kcfi_type !{{[0-9]+}} !kcfi_member_tag [[TAG1:![0-9]+]]
define void @a() !type !0 !kcfi_type !10 { ret void }
; CHECK: define void @b() !type !{{[0-9]+}} !kcfi_type !{{[0-9]+}} !kcfi_member_tag [[TAG1]]
define void @b() !type !0 !kcfi_type !10 { ret void }
;; @both belongs to typeid1 and typeid2: another class.
; CHECK: define void @both() !type !{{[0-9]+}} !type !{{[0-9]+}} !kcfi_type !{{[0-9]+}} !kcfi_member_tag [[TAG2:![0-9]+]]
define void @both() !type !0 !type !1 !kcfi_type !10 { ret void }
;; A function visible outside the module is a member, as is a local one whose
;; address is taken; a local one whose address is not taken is not, nor is a
;; function without a KCFI prefix.
; CHECK: define internal void @local_taken() !type !{{[0-9]+}} !kcfi_type !{{[0-9]+}} !kcfi_member_tag [[TAG1]]
define internal void @local_taken() !type !0 !kcfi_type !10 { ret void }
; CHECK: define internal void @local() !type !{{[0-9]+}} !kcfi_type !{{[0-9]+}} {
define internal void @local() !type !0 !kcfi_type !10 { ret void }
; CHECK: define void @unprefixed() !type !{{[0-9]+}} {
define void @unprefixed() !type !0 { ret void }

; CHECK-LABEL: define i1 @test1(
; CHECK: call i1 @llvm.kcfi.member.test(ptr %p, metadata [[TAGS1:![0-9]+]])
define i1 @test1(ptr %p) {
  call void @local()
  %x = call i1 @llvm.type.test(ptr %p, metadata !"typeid1")
  ret i1 %x
}
; CHECK-LABEL: define i1 @test2(
; CHECK: call i1 @llvm.kcfi.member.test(ptr %p, metadata i32 [[T2:-?[0-9]+]])
define i1 @test2(ptr %p) {
  %x = call i1 @llvm.type.test(ptr %p, metadata !"typeid2")
  ret i1 %x
}

;; A type with no members in the module is tested for no tags, so that a
;; function of the type that LTO did not see still reaches its KCFI thunk.
; CHECK-LABEL: define i1 @test3(
; CHECK: call i1 @llvm.kcfi.member.test(ptr %p, metadata [[NONE:![0-9]+]])
define i1 @test3(ptr %p) {
  %x = call i1 @llvm.type.test(ptr %p, metadata !"typeid3")
  ret i1 %x
}

declare i1 @llvm.type.test(ptr, metadata)

; CHECK-DAG: [[NONE]] = !{}{{$}}
; CHECK-DAG: [[TAG1]] = !{i32 [[T1:-?[0-9]+]]}
; CHECK-DAG: [[TAG2]] = !{i32 [[T2]]}
; CHECK-DAG: [[TAGS1]] = !{i32 [[T2]], i32 [[T1]]}

!0 = !{i64 0, !"typeid1"}
!1 = !{i64 0, !"typeid2"}
!10 = !{i32 12345}

!llvm.module.flags = !{!20}
!20 = !{i32 4, !"kcfi-marker", i32 119298566}

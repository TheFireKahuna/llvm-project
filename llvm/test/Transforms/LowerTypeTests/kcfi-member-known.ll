; RUN: opt -S -passes='lowertypetests,function(cfguard)' %s | FileCheck %s

;; A test of a function known to be a member of the type is true, whether or
;; not the function can carry a membership tag. One whose prefix holds the
;; second type of a function that can occupy a vtable slot carries no tag, so
;; the test of its tags would reject it.

target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-windows-itanium"

@table = constant [3 x ptr] [ptr @a, ptr @vf, ptr @other]

define void @a() !type !0 !kcfi_type !10 { ret void }
define void @vf() !type !0 !kcfi_type !10 !kcfi_vfn_type !11 { ret void }
define void @other() !type !1 !kcfi_type !10 { ret void }

; CHECK-LABEL: define i1 @tagged(
; CHECK-NEXT:    ret i1 true
define i1 @tagged() {
  %x = call i1 @llvm.type.test(ptr @a, metadata !"typeid1")
  ret i1 %x
}

; CHECK-LABEL: define i1 @untagged(
; CHECK-NEXT:    ret i1 true
define i1 @untagged() {
  %x = call i1 @llvm.type.test(ptr @vf, metadata !"typeid1")
  ret i1 %x
}

; CHECK-LABEL: define i1 @nonmember(
; CHECK-NEXT:    ret i1 false
define i1 @nonmember() {
  %x = call i1 @llvm.type.test(ptr @other, metadata !"typeid1")
  ret i1 %x
}

declare i1 @llvm.type.test(ptr, metadata)

!0 = !{i64 0, !"typeid1"}
!1 = !{i64 0, !"typeid2"}
!10 = !{i32 12345}
!11 = !{i32 6789}

!llvm.module.flags = !{!20}
!20 = !{i32 4, !"function-type-prefix", i32 119298566}

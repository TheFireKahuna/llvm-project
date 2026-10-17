; RUN: opt -S -passes=lowertypetests -lowertypetests-summary-action=import \
; RUN:   -lowertypetests-read-summary=%S/Inputs/import-kcfi-member.yaml %s | \
; RUN:   FileCheck %s

;; Where KCFI checks go through per-type thunks, a ThinLTO backend gives each
;; function the membership tag the thin link gave it, and lowers a test of a
;; type resolved by membership to a test of its tags. A type without a
;; resolution has no members in the LTO unit: a test of no tags. A test that
;; only llvm.assume uses is left for devirtualization, and other resolutions
;; are lowered as before.

; CHECK: define internal void @tagged() !kcfi_type !{{[0-9]+}} !guid !{{[0-9]+}} !kcfi_member_tag [[TAG:![0-9]+]] {
; CHECK: define void @untagged() !kcfi_type !{{[0-9]+}} !guid !{{[0-9]+}} {
; CHECK-LABEL: define i1 @member(
; CHECK:   call i1 @llvm.kcfi.member.test(ptr %p, metadata [[TAGS:![0-9]+]])
; CHECK-LABEL: define i1 @none(
; CHECK:   call i1 @llvm.kcfi.member.test(ptr %p, metadata [[NONE:![0-9]+]])
; CHECK-LABEL: define i1 @single(
; CHECK:   icmp eq i64 %1, ptrtoint (ptr @__typeid_typeid2_global_addr to i64)
; CHECK-LABEL: define void @assume(
; CHECK:   call i1 @llvm.type.test(ptr %p, metadata !"typeid3")
; CHECK: [[TAG]] = !{i32 7}
; CHECK: [[TAGS]] = !{i32 7, i32 9}
; CHECK: [[NONE]] = !{}

target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-windows-itanium"

define internal void @tagged() !kcfi_type !10 !guid !{i64 1001} { ret void }
define void @untagged() !kcfi_type !10 !guid !{i64 1002} { ret void }

define i1 @member(ptr %p) {
  %x = call i1 @llvm.type.test(ptr %p, metadata !"typeid1")
  ret i1 %x
}
define i1 @none(ptr %p) {
  %x = call i1 @llvm.type.test(ptr %p, metadata !"typeid3")
  ret i1 %x
}
define i1 @single(ptr %p) {
  %x = call i1 @llvm.type.test(ptr %p, metadata !"typeid2")
  ret i1 %x
}
define void @assume(ptr %p) {
  %x = call i1 @llvm.type.test(ptr %p, metadata !"typeid3")
  call void @llvm.assume(i1 %x)
  ret void
}
declare i1 @llvm.type.test(ptr, metadata)
declare void @llvm.assume(i1)

!10 = !{i32 12345}
!llvm.module.flags = !{!20}
!20 = !{i32 4, !"function-type-prefix", i32 119298566}

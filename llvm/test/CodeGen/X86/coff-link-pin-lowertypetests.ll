; RUN: opt -passes=lowertypetests %s -o - | \
; RUN:   llc -mtriple=x86_64-unknown-windows-itanium -data-sections | \
; RUN:   FileCheck %s

; Vtables merged by LowerTypeTests keep their placement and their exports:
; the combined global carries the required pin, in its own section, and each
; alias keeps the original's DLL storage class.

target datalayout = "e-m:w-p:64:64-i64:64-n32:64-S128"

$vtT = comdat any

; CHECK:      .section .rdata,"dr",one_only,[[G:__unnamed_[0-9]+]]
; CHECK:      .p2align 6
; CHECK-NEXT: [[G]]:
; CHECK-NEXT: .linkpin [[G]], 12, 384, required
; CHECK:      .ascii " -export:vtA,data"
; CHECK:      .ascii " -export:vtB,data"
; CHECK:      vtT = [[G]]+24
; CHECK:      vtA = [[G]]+64
; CHECK:      vtB = [[G]]+112

; An ordinary vtable: address point at 16, align 64.
@vtA = dllexport constant [4 x ptr] [ptr null, ptr null, ptr @fa, ptr @fa], align 64, !type !0
; Address point at 32, with a pin that is not required.
@vtB = dllexport constant [5 x ptr] [ptr null, ptr null, ptr null, ptr null, ptr @fb], align 8, !type !1, !pin !10
; A tagged vtable: a required pin of modulus 4096.
@vtT = linkonce_odr constant [3 x ptr] [ptr null, ptr null, ptr @fb], comdat, align 8, !type !0, !pin !11

define void @fa(ptr %this) { ret void }
define void @fb(ptr %this) { ret void }

define i1 @test(ptr %p) {
  %x = call i1 @llvm.type.test(ptr %p, metadata !"_ZTS1A")
  ret i1 %x
}
declare i1 @llvm.type.test(ptr, metadata)

!0 = !{i64 16, !"_ZTS1A"}
!1 = !{i64 32, !"_ZTS1A"}
!10 = !{i64 32, i64 6, i64 16, i64 0}
!11 = !{i64 16, i64 12, i64 424, i64 1}

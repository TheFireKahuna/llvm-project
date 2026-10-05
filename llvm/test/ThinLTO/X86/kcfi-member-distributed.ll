;; With distributed ThinLTO, the index the thin link writes for each module
;; carries the membership tags its backend needs: the tag of each function it
;; defines, and the tags of each type it tests.

; RUN: rm -rf %t && split-file %s %t && cd %t
; RUN: opt -thinlto-bc -thinlto-split-lto-unit a.ll -o a.bc
; RUN: opt -thinlto-bc -thinlto-split-lto-unit b.ll -o b.bc
; RUN: llvm-lto2 run a.bc b.bc -thinlto-distributed-indexes -o out \
; RUN:   -r a.bc,member,plx -r a.bc,fp,plx -r b.bc,fp, -r b.bc,call,plx \
; RUN:   -r b.bc,main,plx
; RUN: llvm-dis a.bc.thinlto.bc -o a.index.ll
; RUN: llvm-dis b.bc.thinlto.bc -o b.index.ll
; RUN: cat a.index.ll b.index.ll | FileCheck %s

; CHECK:     module: (path: "a.bc"
; CHECK:     gv: (guid: [[#]], summaries: (function: ({{.*}})), kcfiMemberTag: [[#TAG:]])
; CHECK-NOT: typeid:
; CHECK:     module: (path: "b.bc"
; CHECK-NOT: kcfiMemberTag
; CHECK:     typeid: (name: "typeid", summary: (typeTestRes: (kind: members, sizeM1BitWidth: 0, memberTags: ([[#TAG]]))))

;--- a.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-windows-itanium"

@fp = global ptr @member

define void @member() !type !0 !kcfi_type !10 { ret void }

!0 = !{i64 0, !"typeid"}
!10 = !{i32 572662306}
!llvm.module.flags = !{!1, !2, !3}
!1 = !{i32 4, !"kcfi", i32 1}
!2 = !{i32 4, !"kcfi-marker", i32 119298566}
!3 = !{i32 2, !"cfguard", i32 2}

;--- b.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-windows-itanium"

@fp = external global ptr

define void @call(ptr %p) noinline !kcfi_type !10 {
  %t = call i1 @llvm.type.test(ptr %p, metadata !"typeid")
  br i1 %t, label %cont, label %trap
trap:
  call void @llvm.ubsantrap(i8 64)
  unreachable
cont:
  call void %p() [ "kcfi"(i32 572662306) ]
  ret void
}

define void @main() !kcfi_type !10 {
  %p = load volatile ptr, ptr @fp
  call void @call(ptr %p)
  ret void
}

declare i1 @llvm.type.test(ptr, metadata)
declare void @llvm.ubsantrap(i8)

!10 = !{i32 572662306}
!llvm.module.flags = !{!1, !2, !3}
!1 = !{i32 4, !"kcfi", i32 1}
!2 = !{i32 4, !"kcfi-marker", i32 119298566}
!3 = !{i32 2, !"cfguard", i32 2}

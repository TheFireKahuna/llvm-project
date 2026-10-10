; RUN: rm -rf %t && split-file %s %t
; RUN: opt -S -passes=lowertypetests -lowertypetests-summary-action=export \
; RUN:   -lowertypetests-read-summary=%t/summary.ll \
; RUN:   -lowertypetests-write-summary=%t/out.yaml %t/main.ll | FileCheck %s
; RUN: FileCheck --check-prefix=SUMMARY %s < %t/out.yaml

;; Where KCFI checks go through per-type thunks, the thin link checks function
;; types by membership tags. The members of a type include the functions that
;; ThinLTO modules define, which are declarations here, whose address the unit
;; takes; one the link finds visible to a native object or exported would be
;; too. A function the unit only calls, one defined outside the LTO unit and
;; one not live are not members. The members' tags reach the backends in the
;; summary, as does the type's resolution, which lists its tags. A member with
;; local linkage is seen through the alias that promotes it, and its tag is
;; also given to the function the alias names, by which its backend finds it.

; CHECK: define void @reg() !type !{{[0-9]+}} !kcfi_type !{{[0-9]+}} !kcfi_member_tag [[TAG:![0-9]+]]
; CHECK: call i1 @llvm.kcfi.member.test(ptr %p, metadata i32 [[T:-?[0-9]+]])
; CHECK: declare !type !{{[0-9]+}} !guid !{{[0-9]+}} void @thin_a()
; CHECK-NOT: !kcfi_member_tag
; CHECK: [[TAG]] = !{i32 [[T]]}

; SUMMARY:      TypeIdMap:
; SUMMARY-NEXT:   typeid1:
; SUMMARY-NEXT:     TTRes:
; SUMMARY-NEXT:       Kind:            Members
; SUMMARY:      KCFIMemberTags:
; SUMMARY-NEXT:   - GUID:            1001
; SUMMARY-NEXT:     Tag:             [[#T:]]
; SUMMARY-NEXT:   - GUID:            1005
; SUMMARY-NEXT:     Tag:             [[#T]]
; SUMMARY-NEXT:   - GUID:            1006
; SUMMARY-NEXT:     Tag:             [[#T]]
; SUMMARY-NEXT: TypeIdMemberTags:
; SUMMARY-NEXT:   typeid1:         [ [[#T]] ]
; SUMMARY-NEXT: ...

;--- main.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-windows-itanium"

@table = constant [1 x ptr] [ptr @reg]

define void @reg() !type !0 !kcfi_type !10 { ret void }

define i1 @test(ptr %p) {
  %x = call i1 @llvm.type.test(ptr %p, metadata !"typeid1")
  ret i1 %x
}
declare i1 @llvm.type.test(ptr, metadata)

!cfi.functions = !{!1, !2, !3, !4, !5}
!1 = !{!"thin_a", i8 0, i64 1001, !0}
!2 = !{!"thin_b", i8 0, i64 1002, !0}
!3 = !{!"native", i8 1, i64 1003, !0}
!4 = !{!"dead", i8 0, i64 1004, !0}
!5 = !{!"local.promoted", i8 0, i64 1005, !0}

!0 = !{i64 0, !"typeid1"}
!10 = !{i32 12345}
!llvm.module.flags = !{!20, !21}
!20 = !{i32 4, !"function-type-prefix", i32 119298566}
!21 = !{i32 4, !"kcfi-image", !"a.dll"}

;--- summary.ll
^0 = module: (path: "thin.o", hash: (0, 0, 0, 0, 0))
^1 = gv: (guid: 42, summaries: (function: (module: ^0, flags: (live: 1), insts: 1, refs: (^2, ^5), typeIdInfo: (typeTests: (14276520915468743435, 15427464259790519041)))))
^2 = gv: (guid: 1001, summaries: (function: (module: ^0, flags: (live: 1), insts: 1)))
^3 = gv: (guid: 1002, summaries: (function: (module: ^0, flags: (live: 1), insts: 1)))
^4 = gv: (guid: 1004, summaries: (function: (module: ^0, flags: (live: 0), insts: 1)))
^5 = gv: (guid: 1005, summaries: (alias: (module: ^0, flags: (live: 1), aliasee: ^6)))
^6 = gv: (guid: 1006, summaries: (function: (module: ^0, flags: (linkage: internal, live: 1), insts: 1)))

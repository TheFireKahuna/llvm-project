; RUN: llc -mtriple=x86_64-unknown-windows-itanium -O0 \
; RUN:   -x86-wineh-unwindv2-instruction-count-threshold=4 < %s \
; RUN:   | FileCheck %s --implicit-check-not="Record kind: S_SEPCODE"

; Version 2 unwind info cuts each function into parts, one in its own section
; after its first epilogue and one in the cleanup funclet in the cold section.
; The procedure's range covers the parts in its own section; those in the cold
; section lie together, one range of separated code with one line table.

; CHECK:      cut.__part.1:
; CHECK:      cut.cold:
; CHECK:      [[CUT:.LBB_END0_[0-9]+]]:
; CHECK-NEXT:   .text
; CHECK-NEXT: .Lfunc_end0:
; CHECK:      cutcleanup.cold:
; CHECK:      cutcleanup.__part.1:
; CHECK:      [[CLEANUP:.LBB_END1_[0-9]+]]:
; CHECK:        .long .Lfunc_end0-cut # Code size
; CHECK:        Record kind: S_SEPCODE
; CHECK-NEXT:   .long 0 # PtrParent
; CHECK-NEXT:   .long 0 # PtrEnd
; CHECK-NEXT:   .long [[CUT]]-cut.cold # Code size
; CHECK:        .cv_linetable 0, cut, .Lfunc_end0
; CHECK-NEXT:   .cv_linetable 0, cut.cold, [[CUT]]
; CHECK:        .long .Lfunc_end1-cutcleanup # Code size
; CHECK:        Record kind: S_SEPCODE
; CHECK-NEXT:   .long 0 # PtrParent
; CHECK-NEXT:   .long 0 # PtrEnd
; CHECK-NEXT:   .long [[CLEANUP]]-cutcleanup.cold # Code size
; CHECK:        .cv_linetable 1, cutcleanup, .Lfunc_end1
; CHECK-NEXT:   .cv_linetable 1, cutcleanup.cold, [[CLEANUP]]
; CHECK-NOT:    .cv_linetable

declare i32 @g2(i32)
declare void @may_throw()
declare i32 @__gxx_personality_seh0(...)

define i32 @cut(i32 %x) uwtable personality ptr @__gxx_personality_seh0 !dbg !5 {
entry:
  %c = icmp eq i32 %x, 7, !dbg !10
  br i1 %c, label %a, label %b, !dbg !10
a:
  %r = tail call i32 @g2(i32 1), !dbg !11
  ret i32 %r, !dbg !11
b:
  invoke void @may_throw() to label %cont unwind label %cs, !dbg !12
cs:
  %s = catchswitch within none [label %catch] unwind to caller, !dbg !12
catch:
  %p = catchpad within %s [ptr null], !dbg !12
  catchret from %p to label %cont, !dbg !12
cont:
  %y = call i32 @g2(i32 2), !dbg !13
  %z = call i32 @g2(i32 %y), !dbg !13
  ret i32 %z, !dbg !13
}

define void @cutcleanup(i1 %c) uwtable personality ptr @__gxx_personality_seh0 !dbg !6 {
entry:
  invoke void @may_throw() to label %done unwind label %cleanup, !dbg !14
cleanup:
  %cp = cleanuppad within none [], !dbg !15
  br i1 %c, label %l, label %r, !dbg !15
l:
  call void @may_throw() [ "funclet"(token %cp) ], !dbg !15
  cleanupret from %cp unwind to caller, !dbg !15
r:
  call void @may_throw() [ "funclet"(token %cp) ], !dbg !16
  call void @may_throw() [ "funclet"(token %cp) ], !dbg !16
  call void @may_throw() [ "funclet"(token %cp) ], !dbg !16
  cleanupret from %cp unwind to caller, !dbg !16
done:
  ret void, !dbg !14
}

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!2, !3}

!0 = distinct !DICompileUnit(language: DW_LANG_C_plus_plus, file: !1, emissionKind: FullDebug)
!1 = !DIFile(filename: "t.cpp", directory: "/")
!2 = !{i32 2, !"CodeView", i32 1}
!3 = !{i32 2, !"Debug Info Version", i32 3}
!4 = !DISubroutineType(types: !{null})
!5 = distinct !DISubprogram(name: "cut", scope: !1, file: !1, line: 1, type: !4, unit: !0, spFlags: DISPFlagDefinition)
!6 = distinct !DISubprogram(name: "cutcleanup", scope: !1, file: !1, line: 10, type: !4, unit: !0, spFlags: DISPFlagDefinition)
!10 = !DILocation(line: 2, scope: !5)
!11 = !DILocation(line: 3, scope: !5)
!12 = !DILocation(line: 4, scope: !5)
!13 = !DILocation(line: 5, scope: !5)
!14 = !DILocation(line: 11, scope: !6)
!15 = !DILocation(line: 12, scope: !6)
!16 = !DILocation(line: 13, scope: !6)

; RUN: llc -mtriple=x86_64-unknown-windows-itanium -filetype=obj %s -o - | \
; RUN:   llvm-readobj --codeview - | FileCheck %s
; RUN: %if aarch64-registered-target %{ llc -mtriple=aarch64-unknown-windows-itanium -filetype=obj %s -o - | \
; RUN:   llvm-readobj --codeview - | FileCheck %s %}

; The cleanup lies in the cold section, which an S_SEPCODE record after the
; function's names, with no scope of its own. The inlined call there counts
; its code from the first separated code, the variable's range lies in that
; section, and the section has its own lines.

; CHECK:      GlobalProcIdSym {
; CHECK:        DisplayName: f
; CHECK:      InlineSiteSym {
; CHECK:        BinaryAnnotations [
; CHECK-NEXT:     ChangeCodeOffsetBase: 1
; CHECK-NEXT:     ChangeCodeOffsetAndLineOffset: {CodeOffset: 0x{{[0-9A-F]+}}, LineOffset: 1}
; CHECK-NEXT:     ChangeCodeLength: 0x{{[0-9A-F]+}}
; CHECK:        VarName: t
; CHECK:      DefRangeRegisterSym {
; CHECK:          OffsetStart: .text$split+0x{{[0-9A-F]+}}
; CHECK:      InlineSiteEnd {
; CHECK:      ProcEnd {
; CHECK-NEXT:   Kind: S_PROC_ID_END
; CHECK-NEXT: }
; CHECK-NEXT: SepCodeSym {
; CHECK-NEXT:   Kind: S_SEPCODE (0x1132)
; CHECK-NEXT:   PtrParent: 0x0
; CHECK-NEXT:   PtrEnd: 0x0
; CHECK-NEXT:   CodeSize: 0x{{[0-9A-F]+}}
; CHECK-NEXT:   Flags [ (0x0)
; CHECK-NEXT:   ]
; CHECK-NEXT:   CodeOffset: f.cold+0x0
; CHECK-NEXT:   Segment: 0x0
; CHECK-NEXT:   LinkageName: f.cold
; CHECK-NEXT:   ParentCodeOffset: f+0x0
; CHECK-NEXT:   ParentSegment: 0x0
; CHECK-NEXT:   ParentLinkageName: f
; CHECK-NEXT: }
; CHECK-NEXT: ScopeEndSym {
; CHECK-NEXT:   Kind: S_END
; CHECK-NEXT: }
; CHECK:      SubSectionType: Lines
; CHECK:      LinkageName: f
; CHECK:      SubSectionType: Lines
; CHECK:      LinkageName: f.cold

$f = comdat any

declare void @may_throw()
declare i32 @g(i32)
declare i32 @__gxx_personality_seh0(...)

define void @f(i32 %a) comdat personality ptr @__gxx_personality_seh0 !dbg !5 {
entry:
  invoke void @may_throw() to label %cont unwind label %ehcleanup, !dbg !12
cont:
  ret void, !dbg !13
ehcleanup:
  %pad = cleanuppad within none [], !dbg !13
  %t = call i32 @g(i32 %a) [ "funclet"(token %pad) ], !dbg !14
    #dbg_value(i32 %t, !11, !DIExpression(), !15)
  %u = call i32 @g(i32 %t) [ "funclet"(token %pad) ], !dbg !14
  cleanupret from %pad unwind to caller, !dbg !13
}

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!2, !3}

!0 = distinct !DICompileUnit(language: DW_LANG_C_plus_plus, file: !1, isOptimized: true, emissionKind: FullDebug)
!1 = !DIFile(filename: "t.cpp", directory: "/")
!2 = !{i32 2, !"CodeView", i32 1}
!3 = !{i32 2, !"Debug Info Version", i32 3}
!4 = !DISubroutineType(types: !{null})
!5 = distinct !DISubprogram(name: "f", scope: !1, file: !1, line: 4, type: !4, unit: !0, spFlags: DISPFlagDefinition | DISPFlagOptimized)
!9 = distinct !DISubprogram(name: "h", scope: !1, file: !1, line: 1, type: !4, unit: !0, spFlags: DISPFlagDefinition | DISPFlagOptimized, retainedNodes: !{!11})
!10 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)
!11 = !DILocalVariable(name: "t", scope: !9, file: !1, line: 2, type: !10)
!12 = !DILocation(line: 5, scope: !5)
!13 = !DILocation(line: 7, scope: !5)
!14 = !DILocation(line: 2, scope: !9, inlinedAt: !16)
!15 = !DILocation(line: 0, scope: !9, inlinedAt: !16)
!16 = distinct !DILocation(line: 6, scope: !5)

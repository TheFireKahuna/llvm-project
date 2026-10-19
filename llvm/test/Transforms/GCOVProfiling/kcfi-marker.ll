;; With a type prefix marker every function carries a type, whether or not the
;; module checks its own calls, so __llvm_gcov_(writeout|reset|init) have
;; !kcfi_type without the kcfi flag.
; RUN: mkdir -p %t && cd %t
; RUN: opt < %s -S -passes=insert-gcov-profiling \
; RUN:  -mtriple=x86_64-unknown-windows-itanium | FileCheck %s

define dso_local void @empty() !dbg !5 {
entry:
  ret void, !dbg !8
}

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!3, !4, !9, !10}

!0 = distinct !DICompileUnit(language: DW_LANG_C99, file: !1, emissionKind: FullDebug, enums: !2)
!1 = !DIFile(filename: "a.c", directory: "")
!2 = !{}
!3 = !{i32 7, !"Dwarf Version", i32 5}
!4 = !{i32 2, !"Debug Info Version", i32 3}
!5 = distinct !DISubprogram(name: "empty", scope: !1, file: !1, line: 1, type: !6, scopeLine: 1, flags: DIFlagAllCallsDescribed, spFlags: DISPFlagDefinition | DISPFlagOptimized, unit: !0, retainedNodes: !2)
!6 = !DISubroutineType(types: !7)
!7 = !{null}
!8 = !DILocation(line: 2, column: 1, scope: !5)
!9 = !{i32 4, !"function-type-prefix", i32 305419896}
!10 = !{i32 4, !"kcfi-hash", !"xxHash64"}

; CHECK: define internal void @__llvm_gcov_writeout()
; CHECK-SAME: !kcfi_type ![[#TYPE:]]
; CHECK: define internal void @__llvm_gcov_reset()
; CHECK-SAME: !kcfi_type ![[#TYPE]]
; CHECK: define internal void @__llvm_gcov_init()
; CHECK-SAME: !kcfi_type ![[#TYPE]]

; CHECK: ![[#TYPE]] = !{i32 -1522505972}

; REQUIRES: x86, aarch64
; RUN: split-file %s %t
; DEFINE: %{arch} = x86_64
; DEFINE: %{machine} = x64
; DEFINE: %{link} = llc -mtriple=%{arch}-unknown-windows-itanium \
; DEFINE:     -filetype=obj %t/a.ll -o %t/a.obj && \
; DEFINE:   llvm-mc -triple=%{arch}-unknown-windows-itanium -filetype=obj \
; DEFINE:     %t/b.s -o %t/b.obj && \
; DEFINE:   lld-link -dll -noentry -export:f -export:f2 -machine:%{machine} \
; DEFINE:     -debug -opt:ref,icf -pdb:%t/a.pdb -out:%t/a.dll %t/a.obj \
; DEFINE:     %t/b.obj 2>&1 | count 0 && \
; DEFINE:   llvm-pdbutil dump -l -symbols -publics %t/a.pdb
; RUN: %{link} | FileCheck %s --check-prefixes=CHECK,X64 --implicit-check-not=cold
; RUN: llvm-symbolizer --obj=%t/a.dll --relative-address 0x1007 0x1041 | \
; RUN:   FileCheck %s --check-prefix=SYM
; REDEFINE: %{arch} = aarch64
; REDEFINE: %{machine} = arm64
; RUN: %{link} | FileCheck %s --check-prefixes=CHECK,A64 --implicit-check-not=cold
; RUN: llvm-symbolizer --obj=%t/a.dll --relative-address 0x1010 0x1050 | \
; RUN:   FileCheck %s --check-prefix=SYM

; A cold section is attributed to its function by an S_SEPCODE after the
; scope (inline site, ranges and lines at its addresses); folded f and f2 name
; one range; the static cold symbol has no public; lines start after setup.

; The symbolizer gives f for its own code, and h inlined in f for cold code.
; SYM:       f
; SYM-NEXT:  t.cpp:5:0
; SYM-EMPTY:
; SYM-NEXT:  h
; SYM-NEXT:  t.cpp:2:0
; SYM-NEXT:  f
; SYM-NEXT:  t.cpp:6:0

; X64:        0001:00000000-00000012, line/addr entries = 3
; X64:        0001:00000039-0000004C, line/addr entries = 1
; X64-NEXT:   6 0000003D !
; X64:        0001:00000039-0000004C, line/addr entries = 1
; X64-NEXT:   10 0000003D !
; A64:        0001:00000000-00000020, line/addr entries = 3
; A64:        0001:00000048-0000005C, line/addr entries = 1
; A64-NEXT:   6 0000004C !
; A64:        0001:00000048-0000005C, line/addr entries = 1
; A64-NEXT:   10 0000004C !

; CHECK:      S_PUB32 [size = 16] `f`
; CHECK:      S_PUB32 [size = 20] `f2`

; CHECK:      S_GPROC32 [size = 44] `f`
; X64-NEXT:   parent = 0, end = [[#]], addr = 0001:0000, code size = 18
; A64-NEXT:   parent = 0, end = [[#]], addr = 0001:0000, code size = 32
; CHECK:      S_INLINESITE [size = 24]
; CHECK-NEXT: inlinee = 0x1000 (h), parent = [[#]], end = [[#]]
; CHECK-NEXT: 0201 code base 1
; CHECK-NEXT: 0B24 code 0x4 (+0x4) line 1 (+1)
; X64-NEXT:   040F code end 0x13 (+0xF)
; A64-NEXT:   0410 code end 0x14 (+0x10)
; X64:        range start = 0001:0068, length = 8
; A64:        range start = 0001:0084, length = 8
; CHECK:      S_END [size = 4]
; CHECK-NEXT: S_SEPCODE [size = 32]
; CHECK-NEXT: parent = 0, end = [[#]]
; X64-NEXT:   code size = 19, addr = 0001:0057, parent addr = 0001:0000
; A64-NEXT:   code size = 20, addr = 0001:0072, parent addr = 0001:0000
; CHECK-NEXT: flags = none
; CHECK-NEXT: S_END [size = 4]
; CHECK-NEXT: S_GPROC32 [size = 44] `f2`
; CHECK:      S_SEPCODE [size = 32]
; CHECK-NEXT: parent = 0, end = [[#]]
; X64-NEXT:   code size = 19, addr = 0001:0057, parent addr = 0001:0032
; A64-NEXT:   code size = 20, addr = 0001:0072, parent addr = 0001:0032

;--- a.ll
$f = comdat any
$f2 = comdat any

declare void @may_throw()
declare void @may_throw2()
declare void @nt() nounwind
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

define void @f2(i32 %a) comdat personality ptr @__gxx_personality_seh0 !dbg !17 {
entry:
  call void @nt(), !dbg !18
  invoke void @may_throw2() to label %cont unwind label %ehcleanup, !dbg !18
cont:
  ret void, !dbg !18
ehcleanup:
  %pad = cleanuppad within none [], !dbg !18
  %t = call i32 @g(i32 %a) [ "funclet"(token %pad) ], !dbg !18
  %u = call i32 @g(i32 %t) [ "funclet"(token %pad) ], !dbg !18
  cleanupret from %pad unwind to caller, !dbg !18
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
!17 = distinct !DISubprogram(name: "f2", scope: !1, file: !1, line: 9, type: !4, unit: !0, spFlags: DISPFlagDefinition | DISPFlagOptimized)
!18 = !DILocation(line: 10, scope: !17)

;--- b.s
.globl may_throw, may_throw2, nt, g, __gxx_personality_seh0
.text
may_throw:
may_throw2:
nt:
g:
__gxx_personality_seh0:
  ret

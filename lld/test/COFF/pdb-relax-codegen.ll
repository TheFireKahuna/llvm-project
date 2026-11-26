; REQUIRES: x86, aarch64

;; Compiled code through LLD: f's tail call to the next chunk, g, is deleted, and
;; g's shorter jumps, padding, unwind data, procedures and lines follow.

; RUN: llc -mtriple=x86_64-windows-itanium -function-sections -O2 \
; RUN:   -filetype=obj %s -o %t.x64.obj
; RUN: lld-link -dll -noentry -export:f -export:g -debug -pdb:%t.x64.pdb \
; RUN:   %t.x64.obj -out:%t.x64.dll -call-graph-profile-sort:none
; RUN: llvm-objdump -d --no-show-raw-insn %t.x64.dll | FileCheck --check-prefix=X64 %s
; RUN: llvm-pdbutil dump -l -symbols %t.x64.pdb | FileCheck --check-prefix=X64-PDB %s
; RUN: llvm-readobj --unwind %t.x64.dll | FileCheck --check-prefix=X64-UNWIND %s
; RUN: llc -mtriple=aarch64-windows-itanium -function-sections -O2 \
; RUN:   -filetype=obj %s -o %t.a64.obj
; RUN: lld-link -machine:arm64 -dll -noentry -export:f -export:g -debug \
; RUN:   -pdb:%t.a64.pdb %t.a64.obj -out:%t.a64.dll \
; RUN:   -call-graph-profile-sort:none
; RUN: llvm-objdump -d --no-show-raw-insn %t.a64.dll | FileCheck --check-prefix=A64 %s
; RUN: llvm-pdbutil dump -l -symbols %t.a64.pdb | FileCheck --check-prefix=A64-PDB %s
; RUN: llvm-readobj --unwind %t.a64.dll | FileCheck --check-prefix=A64-UNWIND %s

; X64:      180001010 <f>:
; X64-NEXT:   incl %ecx
; X64-NEXT:   nopw %cs:(%rax,%rax)
; X64:      180001020 <g>:
; X64:      18000102e: movl %edi, %ecx
; X64:      180001035: incl %edi
; X64:          jl 0x18000102e
; X64:      18000103b: addq $0x28, %rsp
; X64:      180001041: retq
; X64-EMPTY:
; X64-UNWIND:      StartAddress: (0x180001024)
; X64-UNWIND-NEXT: EndAddress: (0x180001041)

; X64-PDB:      0001:00000010-00000012, line/addr entries = 1
; X64-PDB-NEXT: 2 00000010 !
; X64-PDB:      0001:00000020-00000042, line/addr entries = 5
; X64-PDB-NEXT: 6 00000020 ! 7 00000020 ! 8 0000002E ! 9 00000035 ! 10 00000041 !
; X64-PDB:      S_GPROC32 [size = 44] `f`
; X64-PDB-NEXT: addr = 0001:0016, code size = 2
; X64-PDB:      S_GPROC32 [size = 44] `g`
; X64-PDB-NEXT: addr = 0001:0032, code size = 34

; A64:      180001004 <f>:
; A64-NEXT:   add w0, w0, #0x1
; A64-EMPTY:
; A64-NEXT: 180001008 <g>:
; A64:      180001020: mov w0, w20
; A64:      180001028: add w20, w20, #0x1
; A64:      180001034: ldr x30, [sp, #0x10]
; A64:      180001040: ret
; A64-EMPTY:
; A64-UNWIND: FunctionLength: 48

; A64-PDB:      0001:00000004-00000008, line/addr entries = 1
; A64-PDB-NEXT: 2 00000004 !
; A64-PDB:      0001:00000008-00000044, line/addr entries = 5
; A64-PDB-NEXT: 6 00000008 ! 7 00000008 ! 8 00000020 ! 9 00000028 ! 10 00000034 !
; A64-PDB:      S_GPROC32 [size = 44] `f`
; A64-PDB-NEXT: addr = 0001:0004, code size = 4
; A64-PDB:      S_GPROC32 [size = 44] `g`
; A64-PDB-NEXT: addr = 0001:0008, code size = 60

define dso_local void @ext(i32 %x) noinline {
  ret void
}

define dso_local void @f(i32 %n) !dbg !6 {
  %m = add i32 %n, 1, !dbg !9
  tail call void @g(i32 %m), !dbg !10
  ret void, !dbg !11
}

define dso_local void @g(i32 %n) !dbg !12 {
entry:
  %e = icmp sgt i32 %n, 0, !dbg !13
  br i1 %e, label %loop, label %exit, !dbg !13
loop:
  %i = phi i32 [ 0, %entry ], [ %i1, %loop ]
  call void @ext(i32 %i), !dbg !14
  %i1 = add i32 %i, 1, !dbg !15
  %c = icmp slt i32 %i1, %n, !dbg !15
  br i1 %c, label %loop, label %exit, !dbg !15
exit:
  ret void, !dbg !16
}

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!3, !4}

!0 = distinct !DICompileUnit(language: DW_LANG_C99, file: !1, producer: "clang", isOptimized: true, runtimeVersion: 0, emissionKind: FullDebug, enums: !2)
!1 = !DIFile(filename: "t.c", directory: "/")
!2 = !{}
!3 = !{i32 2, !"CodeView", i32 1}
!4 = !{i32 2, !"Debug Info Version", i32 3}
!6 = distinct !DISubprogram(name: "f", scope: !1, file: !1, line: 1, type: !7, isLocal: false, isDefinition: true, scopeLine: 1, isOptimized: true, unit: !0, retainedNodes: !2)
!7 = !DISubroutineType(types: !8)
!8 = !{null}
!9 = !DILocation(line: 2, scope: !6)
!10 = !DILocation(line: 3, scope: !6)
!11 = !DILocation(line: 4, scope: !6)
!12 = distinct !DISubprogram(name: "g", scope: !1, file: !1, line: 6, type: !7, isLocal: false, isDefinition: true, scopeLine: 6, isOptimized: true, unit: !0, retainedNodes: !2)
!13 = !DILocation(line: 7, scope: !12)
!14 = !DILocation(line: 8, scope: !12)
!15 = !DILocation(line: 9, scope: !12)
!16 = !DILocation(line: 10, scope: !12)

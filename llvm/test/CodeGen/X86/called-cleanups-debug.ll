; RUN: llc -mtriple=x86_64-unknown-windows-itanium < %s | FileCheck %s
; RUN: llc -mtriple=x86_64-unknown-windows-itanium -O0 < %s | FileCheck %s --check-prefix=O0
; RUN: sed 's/"CodeView", i32 1/"Dwarf Version", i32 5/' %s \
; RUN:   | llc -mtriple=x86_64-unknown-windows-itanium -filetype=obj \
; RUN:   | llvm-dwarfdump --debug-info - | FileCheck %s --check-prefix=DWARF

; A cleanup funclet that an unwinder calls reaches its function's frame from
; the establisher frame, in RDX and then wherever its code keeps it, and its
; stack pointer is its own. A variable of the frame is described from the
; stack pointer in the function's code and from that register in the
; funclet's, at the offset the funclet's code uses. A variable that the
; function has in a callee-saved register is described in the funclet too,
; which is entered with the function's callee-saved registers.

; CHECK-LABEL: f:
; CHECK:         retq
; CHECK-NEXT:  [[FUNCLET:.Ltmp[0-9]+]]:
; CHECK-NEXT:    .seh_handlerdata
; CHECK:       "?dtor$2@?0?f@4HA":
; CHECK:         leaq [[#OFF:]](%rdx), %rcx
; CHECK-NEXT:    jmp dtor
; CHECK-NEXT:  [[END:.Ltmp[0-9]+]]:
; CHECK:         .asciz "a"
; CHECK:         .cv_def_range .Ltmp{{[0-9]+}} [[FUNCLET]], frame_ptr_rel, [[#OFF]]
; CHECK-NEXT:    .cv_def_range [[FUNCLET]] [[END]], reg_rel, 331, 0, [[#OFF]]
; CHECK-NOT:     .cv_def_range
; CHECK:         .asciz "x"
; CHECK:         .cv_def_range .Ltmp{{[0-9]+}} .Ltmp{{[0-9]+}} [[FUNCLET]] .Lfunc_end0, reg, 23
; CHECK-NEXT:    .cv_def_range .Ltmp{{[0-9]+}} [[FUNCLET]], reg, 17
; CHECK-NOT:     .cv_def_range

; Unoptimized code keeps the frame pointer, from which the variable is at one
; offset in all of the function.
; O0-LABEL: f:
; O0:         .seh_setframe %rbp
; O0:         .asciz "a"
; O0:         .cv_def_range .Ltmp{{[0-9]+}} .Ltmp{{[0-9]+}}, frame_ptr_rel, -4
; O0-NEXT:    .short

; DWARF:      DW_TAG_variable
; DWARF:        DW_AT_location
; DWARF-NEXT:     DW_OP_breg7 RSP+[[#OFF:]]
; DWARF-NEXT:     DW_OP_breg1 RDX+[[#OFF]])
; DWARF-NEXT:   DW_AT_name ("a")

declare void @may_throw()
declare i32 @get()
declare void @ctor(ptr)
declare void @dtor(ptr) nounwind
declare i32 @__gxx_personality_seh0(...)

define i32 @f() personality ptr @__gxx_personality_seh0 !dbg !6 {
entry:
  %a = alloca i32
    #dbg_declare(ptr %a, !11, !DIExpression(), !13)
  %x = call i32 @get(), !dbg !13
    #dbg_value(i32 %x, !12, !DIExpression(), !13)
  call void @ctor(ptr %a), !dbg !13
  invoke void @may_throw() to label %cont unwind label %ehcleanup, !dbg !13
cont:
  call void @dtor(ptr %a), !dbg !13
  ret i32 %x, !dbg !13
ehcleanup:
  %pad = cleanuppad within none [], !dbg !13
  call void @dtor(ptr %a) [ "funclet"(token %pad) ], !dbg !13
  cleanupret from %pad unwind to caller, !dbg !13
}

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!3, !4}

!0 = distinct !DICompileUnit(language: DW_LANG_C_plus_plus_14, file: !1, producer: "clang", isOptimized: true, emissionKind: FullDebug)
!1 = !DIFile(filename: "t.cpp", directory: "/")
!3 = !{i32 2, !"CodeView", i32 1}
!4 = !{i32 2, !"Debug Info Version", i32 3}
!6 = distinct !DISubprogram(name: "f", scope: !1, file: !1, line: 1, type: !7, scopeLine: 1, spFlags: DISPFlagDefinition | DISPFlagOptimized, unit: !0, retainedNodes: !10)
!7 = !DISubroutineType(types: !8)
!8 = !{!9}
!9 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)
!10 = !{!11, !12}
!11 = !DILocalVariable(name: "a", scope: !6, file: !1, line: 2, type: !9)
!12 = !DILocalVariable(name: "x", scope: !6, file: !1, line: 3, type: !9)
!13 = !DILocation(line: 4, scope: !6)

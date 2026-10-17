; RUN: llc < %s -mtriple=x86_64-unknown-windows-itanium | FileCheck %s
; RUN: llc < %s -mtriple=x86_64-pc-windows-ntposix | FileCheck %s
; RUN: llc < %s -mtriple=x86_64-unknown-windows-itanium -enable-new-pm | \
; RUN:   FileCheck %s
; RUN: llc < %s -mtriple=x86_64-unknown-windows-itanium -verify-machineinstrs \
; RUN:   -o /dev/null
; RUN: llc < %s -mtriple=x86_64-unknown-windows-itanium -O0 | \
; RUN:   FileCheck %s --check-prefix=NONE
; RUN: llc < %s -mtriple=x86_64-pc-windows-msvc | \
; RUN:   FileCheck %s --check-prefix=NONE
; RUN: llc < %s -mtriple=x86_64-unknown-windows-itanium \
; RUN:   -mattr=+retpoline-indirect-calls | FileCheck %s --check-prefix=NONE

; A call through the register that a load of an import pointer fills, such as
; one that machine LICM leaves when it hoists the load out of a loop, is
; described to the linker with the load, and padded to the length of the same
; call through the pointer: the linker makes it direct where the function is
; in the image, and a call through the pointer where it is delay-loaded. A
; call through the pointer that a guarding test's register still holds becomes
; a call through the register, described the same way.

; NONE-NOT: .linkfused

declare dllimport void @imp(i32)
declare dllimport void @take(ptr)
declare extern_weak void @weak()

; Only the calls read the register, so the load is described as one the linker
; may dispose of.
define void @loop(i32 %n) {
; CHECK-LABEL: loop:
; CHECK:       [[LOAD:\.Lfused[0-9]+]]:
; CHECK-NEXT:    .linkfusedread
; CHECK-NEXT:    movq __imp_imp(%rip), [[REG:%[a-z0-9]+]]
; CHECK:       .LBB0_2:
; CHECK:         .linkfusedcall [[LOAD]], [[REG]]
; CHECK-NEXT:    cs
; CHECK-NEXT:    cs
; CHECK-NEXT:    cs
; CHECK:         callq *[[REG]]
entry:
  %c = icmp sgt i32 %n, 0
  br i1 %c, label %body, label %exit
body:
  %i = phi i32 [ 0, %entry ], [ %i1, %body ]
  call void @imp(i32 %i)
  %i1 = add i32 %i, 1
  %d = icmp eq i32 %i1, %n
  br i1 %d, label %exit, label %body
exit:
  ret void
}

; The pointer is also an argument, read through another register, so the load
; stays whatever the call becomes.
define void @argument(i32 %n) {
; CHECK-LABEL: argument:
; CHECK:       [[LOAD:\.Lfused[0-9]+]]:
; CHECK-NEXT:    movq __imp_take(%rip), [[REG:%[a-z0-9]+]]
; CHECK:         movq [[REG]], %r{{cx|di}}
; CHECK-NEXT:    .linkfusedcall [[LOAD]], [[REG]]
; CHECK:         callq *[[REG]]
entry:
  br label %body
body:
  %i = phi i32 [ 0, %entry ], [ %i1, %body ]
  call void @take(ptr @take)
  %i1 = add i32 %i, 1
  %d = icmp eq i32 %i1, %n
  br i1 %d, label %exit, label %body
exit:
  ret void
}

; The test leaves the pointer in rax, through which the linker may call an
; absent function's zero, or one in another image, as a jump too.
define void @guarded() {
; CHECK-LABEL: guarded:
; CHECK:       [[LOAD:\.Lfused[0-9]+]]:
; CHECK-NEXT:    movq __imp_weak(%rip), %rax
; CHECK-NEXT:    testq %rax, %rax
; CHECK:         .linkfusedcall [[LOAD]], %rax
; CHECK-NEXT:    cs
; CHECK-NEXT:    cs
; CHECK-NEXT:    cs
; CHECK-NEXT:    cs
; CHECK-NEXT:    callq *%rax
  %c = icmp ne ptr @weak, null
  br i1 %c, label %call, label %exit
call:
  call void @weak()
  br label %exit
exit:
  ret void
}

define void @guarded_tail() {
; CHECK-LABEL: guarded_tail:
; CHECK:       [[LOAD:\.Lfused[0-9]+]]:
; CHECK-NEXT:    movq __imp_weak(%rip), %rax
; CHECK:         .linkfusedcall [[LOAD]], %rax
; CHECK-NEXT:    rex64 jmpq *%rax
; CHECK-NEXT:    int3
; CHECK-NEXT:    int3
; CHECK-NEXT:    int3
; CHECK-NEXT:    int3
  %c = icmp ne ptr @weak, null
  br i1 %c, label %call, label %exit
call:
  tail call void @weak()
  ret void
exit:
  ret void
}

; A variable said to hold the pointer in the register the linker may leave
; empty is undefined there.
define void @debug(i32 %n) !dbg !5 {
; CHECK-LABEL: debug:
; CHECK:         .linkfusedread
; CHECK-NEXT:    movq __imp_imp(%rip), %r{{[a-z0-9]+}}
; CHECK:         callq *%r{{[a-z0-9]+}}
; CHECK:         #DEBUG_VALUE: debug:fp <- undef
entry:
  br label %body
body:
  %i = phi i32 [ 0, %entry ], [ %i1, %body ]
    #dbg_value(ptr @imp, !9, !DIExpression(), !11)
  call void @imp(i32 %i), !dbg !11
  %i1 = add i32 %i, 1
  %d = icmp eq i32 %i1, %n
  br i1 %d, label %exit, label %body
exit:
  ret void
}

; The call through the register names no function, but the function it calls
; does not unwind, so the call needs no entry between the invokes around it.
define void @nounwind_call(i32 %n) personality ptr @__gxx_personality_seh0 {
; CHECK-LABEL: nounwind_call:
; CHECK:         .linkfusedcall
; CHECK:       GCC_except_table{{[0-9]+}}:
; CHECK:       .Lcst_begin{{[0-9]+}}:
; CHECK-NEXT:    .uleb128 .Ltmp{{[0-9]+}}-.Lfunc_begin{{[0-9]+}}
; CHECK-NEXT:    .uleb128 .Ltmp{{[0-9]+}}-.Ltmp{{[0-9]+}}
; CHECK-NEXT:    .uleb128 .Ltmp{{[0-9]+}}-.Lfunc_begin{{[0-9]+}}
; CHECK-NEXT:    .byte 0
; CHECK-NEXT:  .Lcst_end{{[0-9]+}}:
entry:
  br label %body
body:
  %i = phi i32 [ 0, %entry ], [ %i1, %cont ]
  invoke void @may_throw()
          to label %cont unwind label %lpad
cont:
  call void @nothrow(i32 %i)
  %i1 = add i32 %i, 1
  %d = icmp eq i32 %i1, %n
  br i1 %d, label %exit, label %body
lpad:
  %lp = landingpad { ptr, i32 } cleanup
  ret void
exit:
  ret void
}

declare void @may_throw()
declare dllimport void @nothrow(i32) nounwind
declare i32 @__gxx_personality_seh0(...)

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!3, !4}
!0 = distinct !DICompileUnit(language: DW_LANG_C99, file: !1, emissionKind: FullDebug)
!1 = !DIFile(filename: "t.c", directory: "/")
!3 = !{i32 2, !"Debug Info Version", i32 3}
!4 = !{i32 2, !"CodeView", i32 1}
!5 = distinct !DISubprogram(name: "debug", scope: !1, file: !1, line: 1, type: !6, unit: !0, spFlags: DISPFlagDefinition)
!6 = !DISubroutineType(types: !{null})
!9 = !DILocalVariable(name: "fp", scope: !5, file: !1, line: 2, type: !10)
!10 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !6, size: 64)
!11 = !DILocation(line: 3, scope: !5)

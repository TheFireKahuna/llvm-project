; RUN: llc < %s -mtriple=x86_64-unknown-windows-itanium | FileCheck %s

; The entry of an overridable default as clang emits it: the executable's base
; through the TEB and PEB, then one compare of the slot's tag, falling through
; to the default with no taken branch. On the replacing path, the target is
; base + rva in RAX, and the musttail call becomes a jump through the Control
; Flow Guard dispatch, which preserves every argument register.

; CHECK-LABEL: _Znwy:
; CHECK:       movq %gs:96, %[[PEB:r[a-z0-9]+]]
; CHECK-NEXT:  movq 16(%[[PEB]]), %[[BASE:r[a-z0-9]+]]
; CHECK-NEXT:  cmpl $-975067149, 64(%[[BASE]])
; CHECK-NEXT:  je .[[REPL:LBB0_[0-9]+]]
; CHECK-NEXT:  .LBB0_{{[0-9]+}}: # %overridable.default
; CHECK-NEXT:  rex64 jmpq *__imp_malloc(%rip)
; CHECK:       .[[REPL]]:
; CHECK-NEXT:  cmpl $2067448533, 128(%[[BASE]])
; CHECK-NEXT:  jne
; CHECK:       movl 68(%[[BASE]]), %eax
; CHECK-NEXT:  addq %[[BASE]], %rax
; CHECK-NEXT:  rex64 jmpq *__guard_dispatch_icall_fptr(%rip)

define weak dso_local ptr @_Znwy(i64 noundef %n) {
entry:
  %0 = load ptr, ptr addrspace(256) inttoptr (i64 96 to ptr addrspace(256)), align 32, !invariant.load !0
  %1 = getelementptr inbounds nuw i8, ptr %0, i64 16
  %2 = load ptr, ptr %1, align 8, !invariant.load !0
  %3 = getelementptr inbounds nuw i8, ptr %2, i64 64
  %4 = load i32, ptr %3, align 8, !invariant.load !0
  %5 = icmp eq i32 %4, -975067149
  br i1 %5, label %overridable.replaced, label %overridable.default, !prof !1

overridable.replaced:
  %6 = getelementptr inbounds nuw i8, ptr %2, i64 128
  %7 = load i32, ptr %6, align 8, !invariant.load !0
  %8 = icmp eq i32 %7, 2067448533
  br i1 %8, label %overridable.forward, label %overridable.default

overridable.forward:
  %9 = getelementptr inbounds nuw i8, ptr %2, i64 68
  %10 = load i32, ptr %9, align 4, !invariant.load !0
  %11 = zext i32 %10 to i64
  %12 = getelementptr inbounds nuw i8, ptr %2, i64 %11
  %13 = musttail call ptr %12(i64 noundef %n)
  ret ptr %13

overridable.default:
  %call = tail call ptr @malloc(i64 noundef %n)
  ret ptr %call
}

declare dllimport ptr @malloc(i64 noundef)

!llvm.module.flags = !{!2}

!0 = !{}
!1 = !{!"branch_weights", i32 1, i32 1048575}
!2 = !{i32 2, !"cfguard", i32 2}

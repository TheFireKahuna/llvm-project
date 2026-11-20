; REQUIRES: aarch64-registered-target
; DEFINE: %{llc} = llc -verify-machineinstrs < %s
; RUN: %{llc} -mtriple=x86_64-unknown-windows-itanium | FileCheck %s --check-prefixes=X64,WIN64 --implicit-check-not=__guard_
; RUN: %{llc} -mtriple=x86_64-pc-windows-ntposix | FileCheck %s --check-prefixes=X64,SYSV --implicit-check-not=__guard_
; RUN: %{llc} -mtriple=aarch64-unknown-windows-itanium | FileCheck %s --check-prefix=A64 --implicit-check-not=__guard_
; RUN: not llc -mtriple=x86_64-unknown-windows-itanium -code-model=large < %s 2>&1 | FileCheck %s --check-prefix=LARGE

;; The CFGuard pass marks the load of an indirect call's target that it proved
;; to come from a constant table of functions of the call's KCFI type, and
;; guards the call as any other. After register allocation, a call whose target
;; stayed in its register from the load loses the guard and the KCFI check;
;; x86-64 makes it through the table where the load is next to it.

@table = internal constant [4 x ptr] [ptr @f, ptr @g, ptr @f, ptr @g]
@handlers = internal constant [256 x ptr] zeroinitializer

declare !kcfi_type !2 void @f()
declare !kcfi_type !2 void @g()

; X64-LABEL: proven:
; X64:         leaq table(%rip), %rax
; X64-NEXT:    callq *(%rax,%r{{cx|di}},8)
; A64-LABEL: proven:
; A64:         ldr x15, [x8, x9, lsl #3]
; A64-NEXT:    blr x15
define void @proven(i64 %i) {
  %idx = and i64 %i, 3
  %p = getelementptr [8 x i8], ptr @table, i64 %idx
  %f = load ptr, ptr %p
  call void %f() [ "kcfi"(i32 12) ]
  ret void
}

; X64-LABEL: proven_tail:
; X64:         rex64 jmpq *(%rax,%r{{cx|di}},8) # TAILCALL
; A64-LABEL: proven_tail:
; A64:         ldr x15, [x8, x9, lsl #3]
; A64-NOT:     blr
; A64:         br x15
define void @proven_tail(i64 %i) {
  %idx = and i64 %i, 3
  %p = getelementptr [8 x i8], ptr @table, i64 %idx
  %f = load ptr, ptr %p
  tail call void %f() [ "kcfi"(i32 12) ]
  ret void
}

;; A System V variadic call passes a count in AL, so the target stays in a
;; register of its own.
; WIN64-LABEL: varargs:
; WIN64:         movq (%rax,%rcx,8), %rax
; WIN64-NEXT:    movl $1, %ecx
; WIN64-NEXT:    callq *%rax
; SYSV-LABEL:  varargs:
; SYSV:          movq (%rax,%rdi,8), %rcx
; SYSV-NEXT:     movl $1, %edi
; SYSV-NEXT:     xorl %eax, %eax
; SYSV-NEXT:     callq *%rcx
define void @varargs(i64 %i) {
  %idx = and i64 %i, 3
  %p = getelementptr [8 x i8], ptr @table, i64 %idx
  %f = load ptr, ptr %p
  call void (...) %f(i32 1) [ "kcfi"(i32 12) ]
  ret void
}

;; A loop would keep the table's address in a callee-saved register, which a
;; callee may spill, so it is computed where the call needs it.
; X64-LABEL: interpret:
; X64-NOT:     handlers
; X64:       # %loop
; X64:         leaq handlers(%rip), %rcx
; X64-NEXT:    movq (%rcx,%rax,8), %rax
; A64-LABEL: interpret:
; A64-NOT:     handlers
; A64:       // %loop
; A64:         adrp x9, handlers
; A64:         ldr x15, [x9, x8, lsl #3]
; A64-NEXT:    blr x15
define void @interpret(ptr %pc, i32 %n) {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %i.next, %loop ]
  %p0 = getelementptr i8, ptr %pc, i32 %i
  %op = load i8, ptr %p0
  %idx = zext i8 %op to i64
  %p = getelementptr [8 x i8], ptr @handlers, i64 %idx
  %f = load ptr, ptr %p
  call void %f(ptr %pc)
  %i.next = add i32 %i, 1
  %done = icmp eq i32 %i.next, %n
  br i1 %done, label %exit, label %loop
exit:
  ret void
}

;; Otherwise x86-64 dispatches: the dispatch function takes the target in RAX,
;; which the KCFI check tests. AArch64's check function leaves the kcfi bundle
;; on the call of the target, whose KCFI check tests it. The same holds for a
;; tail call and an invoke. The large code model has no form of the check.
; LARGE: LLVM ERROR: KCFI checks of calls through the Control Flow Guard dispatch function are not supported in the large code model
; X64-LABEL: unproven:
; X64:         movq %r{{cx|di}}, %rax
; X64-NEXT:    movl $4294967284, %r10d
; X64-NEXT:    addl -4(%rax), %r10d
; X64:         callq *__guard_dispatch_icall_fptr(%rip)
; A64-LABEL: unproven:
; A64:         adrp x8, __guard_check_icall_fptr
; A64-NEXT:    mov x15, x0
; A64-NEXT:    ldr x8, [x8, :lo12:__guard_check_icall_fptr]
; A64-NEXT:    blr x8
; A64-NEXT:    ldur w16, [x0, #-4]
; A64:         cmp w16, w17
; A64-NEXT:    b.eq .Ltmp[[#PASS:]]
; A64-NEXT:    brk #0x8220
; A64-NEXT:  .Ltmp[[#PASS]]:
; A64-NEXT:    blr x0
define void @unproven(ptr %x) {
  call void %x() [ "kcfi"(i32 12) ]
  ret void
}

; X64-LABEL: unproven_tail:
; X64:         addl -4(%rax), %r10d
; X64:         rex64 jmpq *__guard_dispatch_icall_fptr(%rip) # TAILCALL
; A64-LABEL: unproven_tail:
; A64:         adrp x8, __guard_check_icall_fptr
; A64:         ldr x8, [x8, :lo12:__guard_check_icall_fptr]
; A64:         ldur w16, [x0, #-4]
; A64:         br x0
define void @unproven_tail(ptr %x) {
  tail call void %x() [ "kcfi"(i32 12) ]
  ret void
}

; X64-LABEL: unproven_invoke:
; X64:         addl -4(%rax), %r10d
; X64:         callq *__guard_dispatch_icall_fptr(%rip)
; A64-LABEL: unproven_invoke:
; A64:         adrp x8, __guard_check_icall_fptr
; A64:         ldr x8, [x8, :lo12:__guard_check_icall_fptr]
; A64:         ldur w16, [x0, #-4]
; A64:         blr x0
define void @unproven_invoke(ptr %x) personality ptr @__CxxFrameHandler3 {
  invoke void %x() [ "kcfi"(i32 12) ] to label %cont unwind label %cleanup
cont:
  ret void
cleanup:
  %pad = cleanuppad within none []
  cleanupret from %pad unwind to caller
}

declare i32 @__CxxFrameHandler3(...)

!llvm.module.flags = !{!0, !1}
!0 = !{i32 4, !"kcfi", i32 1}
!1 = !{i32 2, !"cfguard", i32 2}
!2 = !{i32 12}

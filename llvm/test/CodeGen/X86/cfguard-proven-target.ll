; RUN: llc -mtriple=x86_64-unknown-windows-itanium -verify-machineinstrs < %s \
; RUN:   | FileCheck %s --check-prefixes=CHECK,WIN64
; RUN: llc -mtriple=x86_64-pc-windows-ntposix -verify-machineinstrs < %s \
; RUN:   | FileCheck %s --check-prefixes=CHECK,SYSV

;; The CFGuard pass marks the load of an indirect call's target that it proved
;; to come from a constant table of functions of the call's KCFI type, and
;; routes the call through the KCFI thunk or the guard function as any other.
;; After register allocation, a call whose target stayed in its register from
;; the load is made directly, through the table where the load is next to it.

@table = internal constant [4 x ptr] [ptr @f, ptr @g, ptr @f, ptr @g]
@mixed = internal constant [4 x ptr] [ptr @f, ptr @h, ptr @f, ptr @g]

declare !kcfi_type !3 void @f()
declare !kcfi_type !3 void @g()
declare !kcfi_type !4 void @h()

; CHECK-LABEL: proven:
; WIN64:         callq *(%rax,%rcx,8)
; SYSV:          callq *(%rax,%rdi,8)
; CHECK-NOT:     __llvm_kcfi_
; CHECK:         retq
define void @proven(i64 %i) {
  %idx = and i64 %i, 3
  %p = getelementptr inbounds [8 x i8], ptr @table, i64 %idx
  %f = load ptr, ptr %p
  call void %f() [ "kcfi"(i32 12) ]
  ret void
}

; CHECK-LABEL: proven_tail:
; WIN64:         rex64 jmpq *(%rax,%rcx,8) # TAILCALL
; SYSV:          rex64 jmpq *(%rax,%rdi,8) # TAILCALL
define void @proven_tail(i64 %i) {
  %idx = and i64 %i, 3
  %p = getelementptr inbounds [8 x i8], ptr @table, i64 %idx
  %f = load ptr, ptr %p
  tail call void %f() [ "kcfi"(i32 12) ]
  ret void
}

;; A table holding a function of another type keeps the check.
; CHECK-LABEL: mismatch:
; CHECK:         movq (%rax,%r{{cx|di}},8), %rax
; CHECK-NEXT:    callq __llvm_kcfi_dispatch_0000000c
define void @mismatch(i64 %i) {
  %idx = and i64 %i, 3
  %p = getelementptr inbounds [8 x i8], ptr @mixed, i64 %idx
  %f = load ptr, ptr %p
  call void %f() [ "kcfi"(i32 12) ]
  ret void
}

;; A call without a KCFI type is proven by any table of functions, and loses
;; its guard dispatch.
; CHECK-LABEL: untyped:
; CHECK:         callq *(%rax,%r{{cx|di}},8)
; CHECK-NOT:     __guard_dispatch_icall_fptr
; CHECK:         retq
define void @untyped(i64 %i) {
  %idx = and i64 %i, 3
  %p = getelementptr inbounds [8 x i8], ptr @mixed, i64 %idx
  %f = load ptr, ptr %p
  call void %f()
  ret void
}

;; A System V variadic call cannot pass the target in RAX, and is checked by
;; the check thunk before the call, which is dropped. Where the load is not
;; next to the call, the target stays in its register.
; CHECK-LABEL: varargs:
; WIN64:         movq (%rax,%rcx,8), %rax
; WIN64-NEXT:    movl $1, %ecx
; WIN64-NEXT:    callq *%rax
; SYSV:          movq (%rax,%rdi,8), %rcx
; SYSV-NEXT:     movl $1, %edi
; SYSV-NEXT:     xorl %eax, %eax
; SYSV-NEXT:     callq *%rcx
; CHECK-NOT:     __llvm_kcfi_
; CHECK:         retq
define void @varargs(i64 %i) {
  %idx = and i64 %i, 3
  %p = getelementptr inbounds [8 x i8], ptr @table, i64 %idx
  %f = load ptr, ptr %p
  call void (...) %f(i32 1) [ "kcfi"(i32 12) ]
  ret void
}

!llvm.module.flags = !{!0, !1, !2}
!0 = !{i32 4, !"kcfi", i32 1}
!1 = !{i32 4, !"function-type-prefix", i32 -559038737}
!2 = !{i32 2, !"cfguard", i32 2}
!3 = !{i32 12}
!4 = !{i32 13}

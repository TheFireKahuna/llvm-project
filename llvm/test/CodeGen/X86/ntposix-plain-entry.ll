; RUN: llc -mtriple=x86_64-windows-ntposix < %s | FileCheck %s
; RUN: llc -mtriple=x86_64-windows-ntposix < %s | FileCheck %s --check-prefix=WEAK
; RUN: llc -mtriple=x86_64-windows-ntposix < %s | FileCheck %s --check-prefix=PREFIX

; A function's plain entry, the entry of its name for callers that pass no
; thread pointer, saves the caller's r14, derives the thread pointer, calls
; the function and restores the caller's r14. Where the function holds no
; thread pointer, the plain entry is the function: one body and one address.
; A refusing plain entry returns the runtime's refusal.

@tv = dso_local thread_local global i32 0
declare ptr @llvm.threadlocal.address.p0(ptr)

; CHECK-LABEL: reads$tp:
; CHECK:       .reloc {{.*}}, IMAGE_REL_LLVM_TLS_BASE
define i32 @reads() "thread-pointer-argument" {
  %p = call ptr @llvm.threadlocal.address.p0(ptr @tv)
  %v = load i32, ptr %p
  ret i32 %v
}

; CHECK-LABEL: unused$tp:
define i32 @unused(i32 %a) "thread-pointer-argument" "thread-pointer-unused" {
  ret i32 %a
}

; CHECK-LABEL: refuses$tp:
define i32 @refuses() "thread-pointer-argument" "thread-pointer-plain-entry"="refusal" {
  %p = call ptr @llvm.threadlocal.address.p0(ptr @tv)
  %v = load i32, ptr %p
  ret i32 %v
}

; CHECK-LABEL: reads:
; CHECK:       pushq %r14
; CHECK:       movl %gs:72
; CHECK:       addq __imp___tp_base$val(%rip), %r14
; CHECK:       callq reads$tp
; CHECK:       popq %r14
; CHECK:       retq

; CHECK-LABEL: refuses:
; CHECK:       movq __imp___tp_try@LOAD_CALL(%rip), %rax

; The alias comes at the end of the module.
; CHECK:       .globl unused
; CHECK:       unused = unused$tp

; A function's address is its plain entry's, and a weak reference's plain
; entry is weak too, null where nothing defines it.
; WEAK-LABEL: takesWeak$tp:
; WEAK:       movq __imp_weakFn(%rip), %rax
; WEAK:       .weak weakFn{{$}}
declare extern_weak i32 @weakFn() "thread-pointer-argument"
define ptr @takesWeak() "thread-pointer-argument" "thread-pointer-unused" {
  ret ptr @weakFn
}

; A function's type prefix and its plain entry's are each named after their
; symbol.
; PREFIX: {{^}}__cfi_typed$tp:
; PREFIX: {{^}}typed$tp:
; PREFIX: {{^}}__cfi_typed:
; PREFIX: {{^}}typed:
define void @typed(ptr %p) "thread-pointer-argument" !kcfi_type !2 {
  %t = call ptr @llvm.threadlocal.address.p0(ptr @tv)
  store ptr %t, ptr %p
  ret void
}

!llvm.module.flags = !{!0, !1}
!0 = !{i32 4, !"kcfi", i32 1}
!1 = !{i32 1, !"function-type-prefix", i32 16}
!2 = !{i32 12345678}

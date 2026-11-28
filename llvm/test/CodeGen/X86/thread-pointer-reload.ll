; RUN: llc -mtriple=x86_64-windows-ntposix -verify-machineinstrs < %s | FileCheck %s
; RUN: llc -mtriple=x86_64-windows-itanium -verify-machineinstrs < %s | FileCheck %s
; RUN: llc -O0 -mtriple=x86_64-windows-ntposix -verify-machineinstrs < %s | FileCheck %s --check-prefix=O0

; A function's thread pointer is r14's value on entry. Where nothing has
; changed r14 it is read there, in place or by a copy, and r14 is not saved;
; elsewhere it is reloaded from r14's save slot and checked, its first word
; being its address, with a fast fail at the end of the function.

declare ptr @llvm.thread.pointer.p0()
declare void @g() "thread-pointer-argument"

; CHECK-LABEL: kept$tp:
; CHECK-NOT:   %r14
; CHECK:       callq g$tp
; CHECK-NOT:   %r14
; CHECK:       callq g$tp
; CHECK-NOT:   %r14
; CHECK:       .seh_endproc
define void @kept() "thread-pointer-argument" {
  %tp = call ptr @llvm.thread.pointer.p0()
  call void @g() [ "threadpointer"(ptr %tp) ]
  call void @g() [ "threadpointer"(ptr %tp) ]
  ret void
}

; CHECK-LABEL: copied$tp:
; CHECK-NEXT:  # %bb.0:
; CHECK-NEXT:  movq %r14, ({{%rdi|%rcx}})
; CHECK-NEXT:  retq
define void @copied(ptr %p) "thread-pointer-argument" {
  %tp = call ptr @llvm.thread.pointer.p0()
  store ptr %tp, ptr %p
  ret void
}

; CHECK-LABEL: reloaded$tp:
; CHECK:       pushq %r14
; CHECK:       #APP
; CHECK:       movq [[SLOT:[0-9]*\(%rsp\)]], %r14
; CHECK-NEXT:  cmpq (%r14), %r14
; CHECK-NEXT:  jne [[FAIL:.Ltmp[0-9]+]]
; CHECK-NEXT:  callq g$tp
; CHECK:       movq [[SLOT]], [[T:%r[a-z0-9]+]]
; CHECK-NEXT:  cmpq ([[T]]), [[T]]
; CHECK-NEXT:  jne [[FAIL]]
; CHECK-NEXT:  movq [[T]], ({{%rsi|%rdx}})
; CHECK:       [[FAIL]]:
; CHECK-NEXT:  movl $2, %ecx
; CHECK-NEXT:  int $41
; CHECK-NEXT:  .Lfunc_end2:
; O0-LABEL:    reloaded$tp:
; O0:          cmpq ({{%r[a-z0-9]+}}), {{%r[a-z0-9]+}}
; O0:          int $41
define void @reloaded(i1 %c, ptr %p) "thread-pointer-argument" {
  %tp = call ptr @llvm.thread.pointer.p0()
  call void asm sideeffect "", "~{r14}"()
  br i1 %c, label %a, label %b
a:
  call void @g() [ "threadpointer"(ptr %tp) ]
  ret void
b:
  store ptr %tp, ptr %p
  ret void
}

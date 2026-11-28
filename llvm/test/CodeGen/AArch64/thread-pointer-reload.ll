; RUN: llc -mtriple=aarch64-windows-ntposix -verify-machineinstrs < %s | FileCheck %s
; RUN: llc -mtriple=aarch64-windows-itanium -verify-machineinstrs < %s | FileCheck %s
; RUN: llc -O0 -mtriple=aarch64-windows-ntposix -verify-machineinstrs < %s | FileCheck %s --check-prefix=O0

; A function's thread pointer is x28's value on entry. Where nothing has
; changed x28 it is read there, in place or by a copy, and x28 is not saved;
; elsewhere it is reloaded from x28's save slot and checked, its first word
; being its address, through a free caller-saved register and keeping the
; flags, with a fast fail at the end of the function.

declare ptr @llvm.thread.pointer.p0()
declare void @g() "thread-pointer-argument"

; CHECK-LABEL: kept$tp:
; CHECK-NOT:   x28
; CHECK:       bl g$tp
; CHECK-NOT:   x28
; CHECK:       bl g$tp
; CHECK-NOT:   x28
; CHECK:       .seh_endproc
define void @kept() "thread-pointer-argument" {
  %tp = call ptr @llvm.thread.pointer.p0()
  call void @g() [ "threadpointer"(ptr %tp) ]
  call void @g() [ "threadpointer"(ptr %tp) ]
  ret void
}

; CHECK-LABEL: copied$tp:
; CHECK-NEXT:  // %bb.0:
; CHECK-NEXT:  str x28, [x0]
; CHECK-NEXT:  ret
define void @copied(ptr %p) "thread-pointer-argument" {
  %tp = call ptr @llvm.thread.pointer.p0()
  store ptr %tp, ptr %p
  ret void
}

; CHECK-LABEL: reloaded$tp:
; CHECK:       str x28, [sp, #-16]!
; CHECK:       //APP
; CHECK:       ldr x28, [sp]
; CHECK-NEXT:  ldr [[S:x[0-9]+]], [x28]
; CHECK-NEXT:  sub [[S]], [[S]], x28
; CHECK-NEXT:  cbnz [[S]], [[FAIL:.Ltmp[0-9]+]]
; CHECK-NEXT:  bl g$tp
; CHECK:       ldr [[T:x[0-9]+]], [sp]
; CHECK-NEXT:  ldr [[S:x[0-9]+]], [[[T]]]
; CHECK-NEXT:  sub [[S]], [[S]], [[T]]
; CHECK-NEXT:  cbnz [[S]], [[FAIL]]
; CHECK-NEXT:  str [[T]], [x1]
; CHECK:       [[FAIL]]:
; CHECK-NEXT:  mov w0, #2
; CHECK-NEXT:  brk #0xf003
; CHECK-NEXT:  .Lfunc_end2:
; O0-LABEL:    reloaded$tp:
; O0:          cbnz
; O0:          brk #0xf003
define void @reloaded(i1 %c, ptr %p) "thread-pointer-argument" {
  %tp = call ptr @llvm.thread.pointer.p0()
  call void asm sideeffect "", "~{x28}"()
  br i1 %c, label %a, label %b
a:
  call void @g() [ "threadpointer"(ptr %tp) ]
  ret void
b:
  store ptr %tp, ptr %p
  ret void
}

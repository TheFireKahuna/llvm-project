; RUN: llc -verify-machineinstrs < %s -mtriple=x86_64-linux -mattr=+avx | FileCheck %s

; A function that calls va_start and forwards its variadic arguments with a
; musttail call saves %xmm0-%xmm7 to the register save area, and nothing more:
; after register allocation the pseudo that saves them also carries implicit
; operands, which are not argument registers.

declare void @llvm.va_start.p0(ptr)
declare void @llvm.va_end.p0(ptr)
declare void @use(ptr)
declare void @callee(ptr, ...)

define void @forward(ptr %fmt, ...) {
  %ap = alloca [24 x i8], align 16
  call void @llvm.va_start.p0(ptr %ap)
  call void @use(ptr %ap)
  call void @llvm.va_end.p0(ptr %ap)
  musttail call void (ptr, ...) @callee(ptr %fmt, ...)
  ret void
}

; CHECK-LABEL: forward:
; CHECK:       testb %al, %al
; CHECK-NEXT:  je .LBB0_2
; CHECK-NEXT:  # %bb.1:
; CHECK-NEXT:  vmovaps %xmm0,
; CHECK-NEXT:  vmovaps %xmm1,
; CHECK-NEXT:  vmovaps %xmm2,
; CHECK-NEXT:  vmovaps %xmm3,
; CHECK-NEXT:  vmovaps %xmm4,
; CHECK-NEXT:  vmovaps %xmm5,
; CHECK-NEXT:  vmovaps %xmm6,
; CHECK-NEXT:  vmovaps %xmm7,
; CHECK-NEXT:  .LBB0_2:

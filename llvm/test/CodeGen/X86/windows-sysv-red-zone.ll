; RUN: llc -mtriple=x86_64-pc-windows-ntposix < %s | FileCheck %s
; RUN: llc -mtriple=x86_64-pc-windows-msvc < %s | FileCheck %s
; RUN: llc -mtriple=x86_64-w64-windows-gnu < %s | FileCheck %s
; RUN: llc -mtriple=x86_64-unknown-linux-gnu < %s \
; RUN:   | FileCheck %s --check-prefix=REDZONE

; Windows builds the frames of its exception and APC dispatchers right below
; the stack pointer of the thread it interrupts, so a System V function there
; has no red zone, NT-POSIX's default convention included, even when no stack
; probe call keeps the stack pointer moving.

; CHECK-LABEL: leaf:
; CHECK:       subq $64, %rsp
; CHECK-NOT:   -{{[0-9]+}}(%rsp
; CHECK:       retq
; REDZONE-LABEL: leaf:
; REDZONE-NOT:   subq
; REDZONE:       -64(%rsp
define x86_64_sysvcc i32 @leaf(i32 %a) nounwind "no-stack-arg-probe" {
  %p = alloca [16 x i32]
  %g = getelementptr [16 x i32], ptr %p, i32 0, i32 %a
  store volatile i32 %a, ptr %g
  %v = load volatile i32, ptr %p
  ret i32 %v
}

; RUN: llc -mtriple=x86_64-windows-ntposix < %s | FileCheck %s --check-prefixes=CHECK,X64
; RUN: %if aarch64-registered-target %{ llc -mtriple=aarch64-windows-ntposix < %s | FileCheck %s --check-prefixes=CHECK,A64 %}

; An imported variable's guard is a declaration that names the variable: its
; address is the variable's record chain with the guard's offset, word 12, and
; each value the chain derives carries its marker.

@v = external thread_local global i32
@"v$guard" = external thread_local global i8, !tls_record !0
declare ptr @llvm.threadlocal.address.p0(ptr)

; CHECK-LABEL: guarded$tp:
; X64:         movq __imp_v$tls@FUSED(%rip)
; A64:         ldr x{{[0-9]+}}, [x{{[0-9]+}}, :fused_lo12:__imp_v$tls]
; CHECK:       .site_fact 8, 0
; CHECK:       .site_fact 8, 1
; CHECK:       .site_fact 8, 2
; CHECK:       .site_fact 8, 12
; X64-NEXT:    movzbl (%r{{[a-z0-9]+}},%r{{[a-z0-9]+}}),
; A64-NEXT:    ldrb w0, [x{{[0-9]+}}, x{{[0-9]+}}]
; CHECK-NOT:   __imp_v$guard
define i8 @guarded() "thread-pointer-argument" {
  %g = call ptr @llvm.threadlocal.address.p0(ptr @"v$guard")
  %b = load i8, ptr %g
  ret i8 %b
}

!0 = !{ptr @v}

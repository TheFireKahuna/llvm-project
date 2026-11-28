; DEFINE: %{check} = FileCheck %s --implicit-check-not='f$guard' --check-prefixes
; RUN: llc -mtriple=x86_64-windows-ntposix < %s | %{check}=CHECK,X64
; RUN: %if aarch64-registered-target %{ llc -mtriple=aarch64-windows-ntposix < %s | %{check}=CHECK,A64 %}

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
; X64-NEXT:    .site_fact 14, 14
; A64-NEXT:    .site_fact 14, 28
; X64-NEXT:    movzbl (%r{{[a-z0-9]+}},%r{{[a-z0-9]+}}),
; A64-NEXT:    ldrb w0, [x{{[0-9]+}}, x{{[0-9]+}}]
; CHECK-NOT:   __imp_v$guard
define i8 @guarded() "thread-pointer-argument" {
  %g = call ptr @llvm.threadlocal.address.p0(ptr @"v$guard")
  %b = load i8, ptr %g
  ret i8 %b
}

; A guard has no symbol of its own, so where the link makes its variable
; final the guard is still reached through the variable's record.

@f = external dso_local thread_local global i32
@"f$guard" = external thread_local global i8, !tls_record !1

; CHECK-LABEL: final$tp:
; X64:         movq __imp_f$tls@FUSED(%rip)
; A64:         ldr x{{[0-9]+}}, [x{{[0-9]+}}, :fused_lo12:__imp_f$tls]
; CHECK:       .site_fact 8, 12
define i8 @final() "thread-pointer-argument" {
  %g = call ptr @llvm.threadlocal.address.p0(ptr @"f$guard")
  %b = load i8, ptr %g
  ret i8 %b
}

; Two chains of one record around its guard's cold call share the load of
; the place word's address and the key's, which the cold part leaves in place
; or copies back, and the record's pointer, which it loads again: each chain
; is stated, so that the linker relaxes them all.

@y = external thread_local global i32
@"y$guard" = external thread_local global i8, !tls_record !2
declare void @init()
declare ptr @get()
declare void @put(ptr, ...)

; CHECK-LABEL: twice$tp:
; CHECK:       .site_fact 8, 12
; CHECK:       {{#|//}} %done
; CHECK-NEXT:  .Ltmp{{[0-9]+}}:
; CHECK-NEXT:  .reloc {{.*}}, IMAGE_REL_LLVM_FUSED_INDEX, __imp_y$tls
; CHECK-NEXT:  .site_fact 8, 0
; X64-NEXT:    movq (%r{{[a-z0-9]+}}), %r
; A64-NEXT:    ldp x{{[0-9]+}}, x{{[0-9]+}}, [x{{[0-9]+}}]
; CHECK:       .site_fact 8, 1
; CHECK:       .site_fact 8, 2
; CHECK:       .site_fact 8, 8
; CHECK-NEXT:  .site_fact 14,
; CHECK:       twice.cold:
; X64:         callq init
; X64:         movq __imp_y$tls@FUSED(%rip)
; A64:         bl init
; A64:         ldr x{{[0-9]+}}, [x{{[0-9]+}}, :fused_lo12:__imp_y$tls]
define i32 @twice(i1 %skip) "thread-pointer-argument" {
entry:
  %b = load i8, ptr @"y$guard"
  %z = icmp eq i8 %b, 0
  br i1 %z, label %init, label %done, !prof !3
init:
  call void @init()
  br label %done
done:
  %p = call ptr @llvm.threadlocal.address.p0(ptr @y)
  store i32 0, ptr @y
  %r = call ptr @get()
  br i1 %skip, label %exit, label %use
exit:
  ret i32 0
use:
  %v = load i32, ptr %p
  call void (ptr, ...) @put(ptr null, ptr null, i32 0, i32 %v)
  br label %exit
}

!0 = !{ptr @v}
!1 = !{ptr @f}
!2 = !{ptr @y}
!3 = !{!"branch_weights", i32 1, i32 1048575}

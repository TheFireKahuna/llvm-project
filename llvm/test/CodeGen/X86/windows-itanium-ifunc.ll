; RUN: llc -mtriple=x86_64-unknown-windows-itanium < %s | FileCheck %s

;; An indirect function is a stub that jumps through its import pointer, which
;; the linker makes a word of read-only data that the image's start-up writes
;; with what the resolver returns, as a link record says; a call reads the
;; pointer too. The stub carries the function's KCFI type, and is the
;; function's one guard-table entry where its address is taken; the resolver
;; and the bodies it returns are not, since only the start-up calls the
;; resolver and only the pointer holds what it returns. A resolver's fallback,
;; which the pointer holds until then, goes in a link record of its own.

; CHECK-LABEL: use:
; CHECK:         callq *__imp_weak(%rip)
; CHECK:         callq *__imp_strong(%rip)

;; A weak ifunc's stub is in a COMDAT of its own.
; CHECK:      .section .text,"xr",discard,weak
; CHECK-NEXT: .globl weak
; CHECK-NEXT: .linkbinding weak, default
; CHECK-NEXT: .def weak;
; CHECK-NEXT: .scl 2;
; CHECK-NEXT: .type 32;
; CHECK-NEXT: .endef
; CHECK-NEXT: .p2align 4
; CHECK-NEXT: __cfi_weak:
; CHECK-NEXT: {disp32} nopl 119298566(%rax)
; CHECK-NEXT: movl $305419896, %eax
; CHECK-NEXT: weak:
; CHECK-NEXT: jmpq *__imp_weak(%rip)
; CHECK-NEXT: .linkifunc weak, weak.resolver
; CHECK-NEXT: .linkifuncfallback weak, weak.b

; CHECK:      .globl strong
; CHECK-NOT:  .linkbinding
; CHECK:      {{^}}strong:
; CHECK-NEXT: jmpq *__imp_strong(%rip)
; CHECK-NEXT: .linkifunc strong, strong.resolver
; CHECK-NOT:  .linkifuncfallback

; CHECK:      .section .gfids$y,"dr"
; CHECK-NEXT: .symidx strong
; CHECK-NEXT: .section .giats$y

$weak.resolver = comdat any

@weak = weak_odr dso_local ifunc i32 (), ptr @weak.resolver, !kcfi_type !0
@strong = dso_local ifunc i32 (), ptr @strong.resolver, !kcfi_type !0
@taken = global ptr @strong

define weak_odr ptr @weak.resolver() comdat !resolver_fallback !4 {
  call void @__cpu_indicator_init()
  %f = load i32, ptr @__cpu_model
  %c = icmp eq i32 %f, 0
  %v = select i1 %c, ptr @weak.a, ptr @weak.b
  ret ptr %v
}

define internal ptr @strong.resolver() {
  ret ptr @strong.a
}

define internal i32 @weak.a() !kcfi_type !0 { ret i32 1 }
define internal i32 @weak.b() !kcfi_type !0 { ret i32 2 }
define internal i32 @strong.a() !kcfi_type !0 { ret i32 3 }

define i32 @use() {
  %a = call i32 @weak()
  %b = call i32 @strong()
  %r = add i32 %a, %b
  ret i32 %r
}

declare void @__cpu_indicator_init()
@__cpu_model = external global i32

!llvm.module.flags = !{!1, !2, !3}
!0 = !{i32 305419896}
!1 = !{i32 4, !"kcfi", i32 1}
!2 = !{i32 4, !"function-type-prefix", i32 119298566}
!3 = !{i32 2, !"cfguard", i32 2}
!4 = !{ptr @weak.b}

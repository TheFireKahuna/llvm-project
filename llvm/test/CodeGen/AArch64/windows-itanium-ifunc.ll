; RUN: llc -mtriple=aarch64-unknown-windows-itanium < %s | FileCheck %s

;; An indirect function is a stub that jumps through its import pointer, which
;; the image's start-up writes with what the resolver returns, as a link record
;; says. The stub carries the function's KCFI type, as data with a marker, and
;; is the function's one guard-table entry where its address is taken.

;; A call reaches the stub.
; CHECK-LABEL: use:
; CHECK:         bl weak
; CHECK:         bl strong

; CHECK:      .section .text,"xr",discard,weak
; CHECK-NEXT: .globl weak
; CHECK-NEXT: .linkbinding weak, default
; CHECK:      __cfi_weak:
; CHECK-NEXT: .ascii "\017\037\200"
; CHECK-NEXT: .word 119298566
; CHECK-NEXT: .byte 184
; CHECK-NEXT: .word 305419896
; CHECK-NEXT: weak:
; CHECK-NEXT: adrp x16, __imp_weak
; CHECK-NEXT: ldr x16, [x16, :lo12:__imp_weak]
; CHECK-NEXT: br x16
; CHECK-NEXT: .linkifunc weak, weak.resolver

; CHECK:      {{^}}strong:
; CHECK-NEXT: adrp x16, __imp_strong
; CHECK-NEXT: ldr x16, [x16, :lo12:__imp_strong]
; CHECK-NEXT: br x16
; CHECK-NEXT: .linkifunc strong, strong.resolver

; CHECK:      .section .gfids$y,"dr"
; CHECK-NEXT: .symidx strong
; CHECK-NEXT: .section .giats$y

$weak.resolver = comdat any

@weak = weak_odr dso_local ifunc i32 (), ptr @weak.resolver, !kcfi_type !0
@strong = dso_local ifunc i32 (), ptr @strong.resolver, !kcfi_type !0
@taken = global ptr @strong

define weak_odr ptr @weak.resolver() comdat {
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

; RUN: llc < %s -mtriple=x86_64-unknown-windows-itanium | \
; RUN:   FileCheck %s --check-prefixes=CHECK,FUSED,WI
; RUN: llc < %s -mtriple=x86_64-pc-windows-ntposix | \
; RUN:   FileCheck %s --check-prefixes=CHECK,FUSED
; RUN: llc < %s -mtriple=x86_64-pc-windows-msvc | \
; RUN:   FileCheck %s --check-prefix=PLAIN --implicit-check-not=FUSED
; RUN: llc < %s -mtriple=x86_64-unknown-windows-itanium -O0 | \
; RUN:   FileCheck %s --check-prefix=O0 --implicit-check-not=FUSED
; RUN: %if aarch64-registered-target %{ llc < %s -mtriple=aarch64-unknown-windows-itanium | \
; RUN:   FileCheck %s --check-prefixes=A64,A64F %}
; RUN: %if aarch64-registered-target %{ llc < %s -mtriple=aarch64-pc-windows-msvc | \
; RUN:   FileCheck %s --check-prefix=A64P --implicit-check-not=FUSED %}

; On Windows Itanium and NT-POSIX an optimized function states the test of an
; import pointer for zero that one je or jne alone reads. x86-64 compares the
; pointer in memory where the test alone reads the load (not under NT-POSIX's
; frame), and marks the test otherwise, as AArch64 marks its cbz or cbnz.

@w = extern_weak global i32
declare extern_weak void @wf()
declare void @other()

define void @alone() {
; CHECK-LABEL: alone:
; WI:            cmpq $0, __imp_wf@FUSED(%rip)
; WI-NEXT:       je
; PLAIN-LABEL: alone:
; O0-LABEL: alone:
; O0:            movq __imp_wf(%rip), %rax
; A64P-LABEL: alone:
; A64-LABEL: alone:
; A64F:          ldr x8, [x8, :fused_lo12:__imp_wf]
; A64F-NEXT:   [[SITE:.Ltmp[0-9]+]]:
; A64F-NEXT:     .reloc [[SITE]], IMAGE_REL_LLVM_FUSED_TEST, __imp_wf
; A64-NEXT:      cbz x8,
entry:
  %c = icmp ne ptr @wf, null
  br i1 %c, label %then, label %else

then:
  call void @other()
  br label %else

else:
  ret void
}

; A result that anything but one jump reads is no fused test.
define i32 @selected(i32 %a, i32 %b) {
; CHECK-LABEL: selected:
; FUSED:         movq __imp_w(%rip), %r[[REG:[a-z]+]]
; FUSED-NEXT:    testq %r[[REG]], %r[[REG]]
; CHECK-NEXT:    cmov
; A64-LABEL: selected:
; A64F:          ldr x8, [x8, :lo12:__imp_w]
; A64-NEXT:      cmp x8, #0
  %c = icmp ne ptr @w, null
  %r = select i1 %c, i32 %a, i32 %b
  ret i32 %r
}

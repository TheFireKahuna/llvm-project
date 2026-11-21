; RUN: llc -mtriple=x86_64-pc-windows-msvc < %s | FileCheck %s
; RUN: %if aarch64-registered-target %{ llc -mtriple=aarch64-pc-windows-msvc < %s | FileCheck %s --check-prefix=A64 %}

;; On COFF, the symbol that marks the type data is local, even for a COMDAT
;; function: a linker treats an external symbol that only a discarded copy
;; defines as undefined, and a prevailing copy may lack the type data.
; CHECK-NOT: .globl __cfi_
; CHECK:     .globl f2
; CHECK-NOT: .globl __cfi_
; CHECK:     __cfi_f2:

define void @f1() !kcfi_type !1 {
  ret void
}

$f2 = comdat any
define linkonce_odr void @f2() comdat !kcfi_type !1 {
  ret void
}

;; Windows' unwinder knows an epilogue only up to its terminator, so a tail
;; call's check precedes the epilogue, and the terminator follows it.
; CHECK-LABEL: f3:
; CHECK:         ud2
; CHECK-NEXT:  .Ltmp{{[0-9]+}}:
; CHECK-NEXT:    .seh_startepilogue
; CHECK:         .seh_endepilogue
; CHECK-NEXT:    rex64 jmpq *%rax
; A64-LABEL:   f3:
; A64:           brk #0x8221
; A64-NEXT:    .Ltmp{{[0-9]+}}:
; A64-NEXT:      .seh_startepilogue
; A64:           .seh_endepilogue
; A64-NEXT:      br x1

declare void @g()

define i64 @f3(ptr %p, i64 %a) {
  call void @g()
  %r = tail call i64 %p(i64 %a) [ "kcfi"(i32 12345678) ]
  ret i64 %r
}

!llvm.module.flags = !{!0}
!0 = !{i32 4, !"kcfi", i32 1}
!1 = !{i32 12345678}

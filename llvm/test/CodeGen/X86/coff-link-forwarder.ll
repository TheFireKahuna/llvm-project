; RUN: llc -mtriple=x86_64-unknown-windows-itanium < %s | FileCheck %s
; RUN: llc -mtriple=x86_64-pc-windows-msvc < %s | FileCheck %s --check-prefix=NONE

;; A function that only forwards to an equivalent imported one tells a linker,
;; which may replace it by that function where the image defines it.

; CHECK:      .linkforwarder fwd, __imp_base
; CHECK:      fwd:
; CHECK-NEXT: # %bb.0:
; CHECK-NEXT: rex64 jmpq *__imp_base(%rip)
; NONE-NOT: .linkforwarder

$fwd = comdat any

define linkonce_odr void @fwd(ptr %this) comdat !forwarder !0 {
  tail call void @base(ptr %this)
  ret void
}

declare dllimport void @base(ptr)

!0 = !{ptr @base}

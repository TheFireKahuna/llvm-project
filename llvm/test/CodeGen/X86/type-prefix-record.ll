; RUN: llc -mtriple=x86_64-unknown-windows-itanium < %s | FileCheck %s
; RUN: sed 's/"function-type-prefix"/"other"/' %s \
; RUN:   | llc -mtriple=x86_64-unknown-windows-itanium | FileCheck %s --check-prefix=NONE
; RUN: llc -mtriple=x86_64-unknown-linux-gnu < %s | FileCheck %s --check-prefix=NONE

;; A COFF object whose module gives functions KCFI prefixes with a marker says
;; so in its link records, so that the linker looks for the prefixes.

; CHECK: .linktypeprefixes
; NONE-NOT: .linktypeprefixes

define void @f() !kcfi_type !0 {
  ret void
}

!llvm.module.flags = !{!1}
!0 = !{i32 12345678}
!1 = !{i32 4, !"function-type-prefix", i32 16}

; RUN: llc -mtriple=x86_64-windows-itanium -filetype=obj < %s | llvm-readobj --symbols - | FileCheck %s
; RUN: llc -mtriple=x86_64-scei-windows-itanium -filetype=obj < %s | llvm-readobj --symbols - | FileCheck %s
; RUN: %if aarch64-registered-target %{ llc -mtriple=aarch64-windows-itanium -filetype=obj < %s | llvm-readobj --symbols - | FileCheck %s %}

; Windows Itanium names a COMDAT function's section .text, as MSVC does, so
; each function's unwind data goes in COMDATs associative to its own code.

$f = comdat any
$g = comdat any

define linkonce_odr void @f() uwtable comdat {
  call void @h()
  ret void
}

define linkonce_odr void @g() uwtable comdat {
  call void @h()
  ret void
}

declare void @h()

; CHECK:      Name: f
; CHECK-NEXT: Value: 0
; CHECK-NEXT: Section: .text ([[F:[0-9]+]])
; CHECK:      Name: .xdata
; CHECK-NOT:  Name:
; CHECK:        Selection: Associative (0x5)
; CHECK-NEXT:   AssocSection: .text ([[F]])
; CHECK:      Name: g
; CHECK-NEXT: Value: 0
; CHECK-NEXT: Section: .text ([[G:[0-9]+]])
; CHECK:      Name: .xdata
; CHECK-NOT:  Name:
; CHECK:        Selection: Associative (0x5)
; CHECK-NEXT:   AssocSection: .text ([[G]])
; CHECK:      Name: .pdata
; CHECK-NOT:  Name:
; CHECK:        Selection: Associative (0x5)
; CHECK-NEXT:   AssocSection: .text ([[F]])
; CHECK:      Name: .pdata
; CHECK-NOT:  Name:
; CHECK:        Selection: Associative (0x5)
; CHECK-NEXT:   AssocSection: .text ([[G]])

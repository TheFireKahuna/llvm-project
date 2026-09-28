; RUN: llc -mtriple=aarch64-windows-itanium -filetype=obj < %s | \
; RUN:   llvm-readobj --symbols - | FileCheck %s
; RUN: llc -mtriple=aarch64-pc-windows-ntposix -filetype=obj < %s | \
; RUN:   llvm-readobj --symbols - | FileCheck %s
; RUN: llc -mtriple=aarch64-w64-windows-gnu -filetype=obj < %s | \
; RUN:   llvm-objdump --headers - | FileCheck %s --check-prefix=GNU

; Windows Itanium and NT-POSIX name a COMDAT function's section .text, as MSVC
; does, so its unwind data goes in associative COMDATs; MinGW names it after
; the function instead.

$f = comdat any

define linkonce_odr void @f() uwtable comdat {
  call void @g()
  ret void
}

declare void @g()

; CHECK:      Name: .xdata
; CHECK:        Selection: Associative (0x5)
; CHECK:      Name: .pdata
; CHECK:        Selection: Associative (0x5)

; GNU: .xdata$f
; GNU: .pdata$f

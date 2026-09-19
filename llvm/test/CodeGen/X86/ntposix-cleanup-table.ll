; RUN: llc -O2 -verify-machineinstrs -mtriple=x86_64-pc-windows-ntposix < %s | FileCheck %s --check-prefix=NTPOSIX
; RUN: llc -O2 -verify-machineinstrs -mtriple=x86_64-pc-windows-itanium < %s | FileCheck %s --check-prefix=ITANIUM

; On NT-POSIX every function's handler data begins with the cleanup table,
; empty here, whose header alone measures it, so a personality is handed the
; LSDA past it; an Itanium triple without the property emits the LSDA alone.

declare void @may_throw()
declare void @drop_a() nounwind
declare i32 @rust_eh_personality(...)

; NTPOSIX-LABEL: plain:
; NTPOSIX: .seh_handlerdata
; NTPOSIX: .section .xdata
; NTPOSIX-NEXT: .p2align 2
; NTPOSIX-NEXT: .byte 1
; NTPOSIX-NEXT: .uleb128 [[END:.Lcleanup_end[0-9]+]]-[[BEGIN:.Lcleanup_begin[0-9]+]]
; NTPOSIX-NEXT: [[BEGIN]]:
; NTPOSIX-NEXT: [[END]]:
; NTPOSIX-NEXT: GCC_except_table0:
; ITANIUM-LABEL: plain:
; ITANIUM: .seh_handlerdata
; ITANIUM: .section .xdata
; ITANIUM-NEXT: .p2align 2
; ITANIUM-NEXT: GCC_except_table0:
define void @plain() personality ptr @rust_eh_personality {
entry:
  invoke void @may_throw() to label %done unwind label %pad
done:
  ret void
pad:
  %e = landingpad { ptr, i32 } cleanup
  call void @drop_a()
  resume { ptr, i32 } %e
}

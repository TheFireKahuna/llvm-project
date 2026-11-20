;; A global in another global's COMDAT is in a section associative to that
;; global's section, selection 5. Windows Itanium and NT-POSIX place a guard
;; variable, and the initialization function of a thread-local variable, with
;; their variable this way.

; RUN: llc -mtriple=x86_64-unknown-windows-itanium -filetype=obj < %s \
; RUN:   | llvm-readobj --symbols - | FileCheck %s
; RUN: llc -mtriple=aarch64-unknown-windows-ntposix -filetype=obj < %s \
; RUN:   | llvm-readobj --symbols - | FileCheck %s

$_Z1x = comdat any
$_Z1t = comdat any

@_Z1x = linkonce_odr global i32 1, comdat
@_ZGV1x = linkonce_odr global i64 0, comdat($_Z1x)
@_Z1t = linkonce_odr thread_local global i32 0, comdat
@_ZGV1t = linkonce_odr thread_local global i64 0, comdat($_Z1t)

@_ZTH1t = linkonce_odr alias void (), ptr @__cxx_global_var_init

define internal void @__cxx_global_var_init() comdat($_Z1t) {
  ret void
}

; CHECK:      Name: .text
; CHECK:      Selection: Associative (0x5)
; CHECK-NEXT: AssocSection: .tls$ ([[#T:]])

; CHECK:      Name: _Z1x
; CHECK-NEXT: Value: 0
; CHECK-NEXT: Section: .data ([[#X:]])

; CHECK:      Name: .bss
; CHECK:      Section: .bss ([[#GVX:]])
; CHECK:      Selection: Associative (0x5)
; CHECK-NEXT: AssocSection: .data ([[#X]])

; CHECK:      Name: _Z1t
; CHECK-NEXT: Value: 0
; CHECK-NEXT: Section: .tls$ ([[#T]])

; CHECK:      Name: .tls$
; CHECK:      Section: .tls$ ([[#GVT:]])
; CHECK:      Selection: Associative (0x5)
; CHECK-NEXT: AssocSection: .tls$ ([[#T]])

; CHECK:      Name: _ZGV1x
; CHECK-NEXT: Value: 0
; CHECK-NEXT: Section: .bss ([[#GVX]])

; CHECK:      Name: _ZGV1t
; CHECK-NEXT: Value: 0
; CHECK-NEXT: Section: .tls$ ([[#GVT]])

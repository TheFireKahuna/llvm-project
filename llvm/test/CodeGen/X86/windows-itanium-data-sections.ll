; RUN: llc -mtriple=x86_64-unknown-windows-itanium < %s | FileCheck %s
; RUN: llc -mtriple=x86_64-pc-windows-ntposix < %s | FileCheck %s
; RUN: llc -mtriple=x86_64-unknown-windows-itanium -data-sections=0 < %s | \
; RUN:   FileCheck --check-prefix=SHARED %s
; RUN: llc -mtriple=x86_64-w64-windows-gnu < %s | \
; RUN:   FileCheck --check-prefix=SHARED %s

;; Windows Itanium and NT-POSIX place each global in a section of its own by
;; default, as -data-sections does, since the linker takes an object's extent
;; from its section and moves a section to bind the words it holds.

; CHECK: .section .data,"dw",one_only,a,unique,0
; CHECK: .section .rdata,"dr",one_only,b,unique,1
; CHECK: .section .bss,"bw",one_only,c,unique,2

; SHARED-NOT: one_only

@a = global i32 1
@b = constant i32 2
@c = internal global i32 0

define ptr @use() {
  ret ptr @c
}

; RUN: llc < %s -mtriple=x86_64-pc-windows-msvc | FileCheck %s

;; An initializer whose key the module does not define, as LTO leaves one of a
;; COMDAT that another object prevails for, is not emitted, so the function
;; table does not list the initializer either. One whose key the module
;; defines is listed.

; CHECK:     .section .gfids$y,"dr"
; CHECK-NEXT: .symidx kept_init
; CHECK-NOT: .symidx dropped_init
; CHECK-NOT: dropped_init

@dropped_key = available_externally global i32 0
@kept_key = global i32 0
@llvm.global_ctors = appending global [2 x { i32, ptr, ptr }] [
  { i32, ptr, ptr } { i32 65535, ptr @dropped_init, ptr @dropped_key },
  { i32, ptr, ptr } { i32 65535, ptr @kept_init, ptr @kept_key }]

declare void @dropped_init()

define internal void @kept_init() {
  store i32 1, ptr @kept_key
  ret void
}

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"cfguard", i32 2}

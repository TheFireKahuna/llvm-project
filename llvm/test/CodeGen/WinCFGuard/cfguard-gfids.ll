; RUN: llc < %s -mtriple=x86_64-pc-windows-msvc | FileCheck %s

; The system calls the language handler an unwind record names without a
; check, so naming it as a personality does not list it in .gfids$y; a
; personality whose address is also taken stays listed. An initializer whose
; key the module does not define, as LTO leaves one of a COMDAT that another
; object prevails for, is not emitted, so .gfids$y does not list it either;
; one whose key the module defines is listed.

; CHECK:      .section .gfids$y
; CHECK-NEXT: .symidx __C_specific_handler
; CHECK-NEXT: .symidx kept_init
; CHECK-NOT:  .symidx

declare i32 @__gxx_personality_seh0(...)
declare i32 @__C_specific_handler(...)
declare void @may_throw()

@taken = global ptr @__C_specific_handler

define void @f() personality ptr @__gxx_personality_seh0 {
  invoke void @may_throw() to label %done unwind label %lpad
lpad:
  %lp = landingpad { ptr, i32 } cleanup
  resume { ptr, i32 } %lp
done:
  ret void
}

define void @seh() personality ptr @__C_specific_handler {
  invoke void @may_throw() to label %done unwind label %pad
pad:
  %cs = catchswitch within none [label %handler] unwind to caller
handler:
  %cp = catchpad within %cs [ptr null]
  catchret from %cp to label %done
done:
  ret void
}

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

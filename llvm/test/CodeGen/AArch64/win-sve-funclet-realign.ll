; RUN: llc -mtriple=aarch64-windows-msvc -mattr=+sve < %s | FileCheck %s
; RUN: llc -mtriple=aarch64-windows-msvc -mattr=+sve -filetype=obj < %s -o /dev/null

; A funclet allocates the scalable locals of its function below its own
; frame record, and frees them itself: its frame pointer is its function's,
; and restoring that sets no stack pointer of the funclet, whether or not the
; function is realigned or has a variable-sized object.

declare void @may_throw()
declare void @use(ptr)
declare i32 @__CxxFrameHandler3(...)

; CHECK-LABEL: "?dtor$2@?0?realigned@4HA":
; CHECK:         stp x29, x30, [sp, #16]
; CHECK-NEXT:    .seh_save_fplr 16
; CHECK-NEXT:    addvl sp, sp, #-1
; CHECK-NEXT:    .seh_allocz 1
; CHECK:         addvl sp, sp, #1
; CHECK-NEXT:    .seh_allocz 1
; CHECK-NEXT:    ldp x29, x30, [sp, #16]
; CHECK-LABEL: "?dtor$2@?0?vla@4HA":
; CHECK:         stp x29, x30, [sp, #16]
; CHECK-NEXT:    .seh_save_fplr 16
; CHECK-NEXT:    addvl sp, sp, #-2
; CHECK-NEXT:    .seh_allocz 2
; CHECK:         addvl sp, sp, #2
; CHECK-NEXT:    .seh_allocz 2
; CHECK-NEXT:    ldp x29, x30, [sp, #16]
define void @realigned(<vscale x 4 x i32> %z) personality ptr @__CxxFrameHandler3 {
entry:
  %a = alloca i32, align 64
  %v = alloca <vscale x 4 x i32>
  store <vscale x 4 x i32> %z, ptr %v
  invoke void @may_throw() to label %cont unwind label %ehcleanup
cont:
  ret void
ehcleanup:
  %pad = cleanuppad within none []
  call void @use(ptr %a) [ "funclet"(token %pad) ]
  call void @use(ptr %v) [ "funclet"(token %pad) ]
  cleanupret from %pad unwind to caller
}

define void @vla(<vscale x 4 x i32> %z, i64 %n) personality ptr @__CxxFrameHandler3 {
entry:
  %v = alloca <vscale x 4 x i32>
  %d = alloca i8, i64 %n
  store <vscale x 4 x i32> %z, ptr %v
  call void @use(ptr %d)
  invoke void @may_throw() to label %cont unwind label %ehcleanup
cont:
  ret void
ehcleanup:
  %pad = cleanuppad within none []
  call void @use(ptr %v) [ "funclet"(token %pad) ]
  cleanupret from %pad unwind to caller
}

; RUN: llc -mtriple=aarch64-unknown-windows-itanium -verify-machineinstrs < %s | FileCheck %s
; RUN: llc -mtriple=aarch64-unknown-windows-itanium -filetype=obj -o /dev/null < %s

; The AArch64 frames of WinEH/called-cleanups.ll whose IR is AArch64's own. The
; object RUN line has MC check each prologue's unwind codes against its code.

declare void @may_throw()
declare void @dtor(ptr) nounwind
declare i32 @__gxx_personality_seh0(...)

; A funclet of a function with a base register takes that from the frame
; pointer as the function's prologue takes it from the stack pointer, and
; saves nothing.
; CHECK-LABEL: base_register:
; CHECK:         add x29, sp, #8
; CHECK-NEXT:    .seh_add_fp 8
; CHECK-NEXT:    .seh_endprologue
; CHECK-NEXT:    sub x9, sp, #32
; CHECK-NEXT:    and sp, x9, #0xffffffffffffffc0
; CHECK:         mov x19, sp
; CHECK:       .LBB0_2:
; CHECK-NEXT:    sub x19, x29, #40
; CHECK-NEXT:    and x19, x19, #0xffffffffffffffc0
; CHECK-NEXT:    add x0, x19, #0
; CHECK-NEXT:    b dtor
define void @base_register(i64 %n) personality ptr @__gxx_personality_seh0 {
entry:
  %a = alloca i32, align 64
  %v = alloca i8, i64 %n
  call void @dtor(ptr %v)
  invoke void @may_throw() to label %cont unwind label %ehcleanup
cont:
  ret void
ehcleanup:
  %pad = cleanuppad within none []
  call void @dtor(ptr %a) [ "funclet"(token %pad) ]
  cleanupret from %pad unwind to caller
}

; Scalable objects lie between the frame record and the base register; a
; funclet allocates none.
; CHECK-LABEL: scalable_frame:
; CHECK:         sub x9, sp, #32
; CHECK-NEXT:    addvl x9, x9, #-2
; CHECK-NEXT:    and sp, x9, #0xffffffffffffffc0
; CHECK:         mov x19, sp
; CHECK:       .LBB1_2:
; CHECK-NEXT:    str x30, [sp, #-16]!
; CHECK-NEXT:    .seh_save_reg_x x30, 16
; CHECK-NEXT:    .seh_endprologue
; CHECK-NEXT:    sub x19, x29, #48
; CHECK-NEXT:    addvl x19, x19, #-2
; CHECK-NEXT:    and x19, x19, #0xffffffffffffffc0
; CHECK-NEXT:    add x0, x19, #0
; CHECK-NEXT:    bl dtor
; CHECK-NEXT:    sub x0, x29, #16
; CHECK-NEXT:    addvl x0, x0, #-1
; CHECK-NEXT:    .seh_startepilogue
; CHECK-NEXT:    ldr x30, [sp], #16
; CHECK-NEXT:    .seh_save_reg_x x30, 16
; CHECK-NEXT:    .seh_endepilogue
; CHECK-NEXT:    b dtor
define void @scalable_frame(<vscale x 4 x i32> %z, i64 %n) "target-features"="+sve" personality ptr @__gxx_personality_seh0 {
entry:
  %a = alloca i32, align 64
  %s = alloca <vscale x 4 x i32>
  %v = alloca i8, i64 %n
  call void @dtor(ptr %v)
  store <vscale x 4 x i32> %z, ptr %s
  invoke void @may_throw() to label %cont unwind label %ehcleanup
cont:
  ret void
ehcleanup:
  %pad = cleanuppad within none []
  call void @dtor(ptr %a) [ "funclet"(token %pad) ]
  call void @dtor(ptr %s) [ "funclet"(token %pad) ]
  cleanupret from %pad unwind to caller
}

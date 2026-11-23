; RUN: llc -mtriple=aarch64-windows-ntposix < %s | FileCheck %s
; RUN: llc -mtriple=aarch64-windows-itanium < %s | FileCheck %s --check-prefixes=CHECK,WI
; RUN: llc -O0 -mtriple=aarch64-windows-ntposix < %s | FileCheck %s
; RUN: llc -mtriple=aarch64-windows-ntposix -filetype=obj < %s \
; RUN:   | llvm-readobj --unwind - | FileCheck %s --check-prefix=UNWIND
; RUN: llc -mtriple=aarch64-windows-itanium -filetype=obj < %s \
; RUN:   | llvm-readobj --unwind - | FileCheck %s --check-prefix=UNWIND

; A 32-bit load through address space 280 is a fault probe, a call site of
; its own whose landing zeroes its register and resumes after it, its one
; catch clause naming the function itself.

declare i32 @__gxx_personality_seh0(...)

; CHECK-LABEL: f:
; CHECK:       .seh_handler __gxx_personality_seh0, @unwind, @except
; CHECK:       [[BEGIN:.Ltmp[0-9]+]]: // EH_LABEL
; CHECK-NEXT:  ldr w8, [x0]
; CHECK-NEXT:  [[END:.Ltmp[0-9]+]]:
; CHECK-NEXT:  [[RESUME:.LBB0_[0-9]+]]:
; CHECK-NEXT:  add w0, w8, w1
; CHECK:       .seh_handlerdata
; WI:          .byte 1 // Scope table version
; CHECK:       .uleb128 [[BEGIN]]-.Lfunc_begin0 // >> Call Site 1 <<
; CHECK-NEXT:  .uleb128 [[END]]-[[BEGIN]]
; CHECK-NEXT:  .uleb128 [[LANDING:.Ltmp[0-9]+]]-f.cold
; CHECK-NEXT:  .byte 1 // On action: 1
; CHECK:       [[TI:.Ltmp[0-9]+]]: // TypeInfo 1
; CHECK-NEXT:  .word __imp_f-[[TI]]
; CHECK:       [[LANDING]]: // EH_LABEL
; CHECK-NEXT:  mov w8, #0 // =0x0
; CHECK:       b [[RESUME]]
; A leaf with a probe has a prologue of no instruction, so that its function
; has unwind information that names its handler.
; UNWIND:      Function: f (0x0)
; UNWIND:      ExceptionData: Yes
; UNWIND:      Prologue [
; UNWIND-NEXT:   0xe4 ; end
; UNWIND:      Routine: __gxx_personality_seh0
define i32 @f(ptr addrspace(280) %p, i32 %x) personality ptr @__gxx_personality_seh0 {
  %v = load volatile i32, ptr addrspace(280) %p
  %a = add i32 %v, %x
  ret i32 %a
}

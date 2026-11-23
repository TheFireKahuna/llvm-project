; RUN: llc -mtriple=x86_64-windows-ntposix < %s | FileCheck %s --check-prefixes=CHECK,SYSV
; RUN: llc -mtriple=x86_64-windows-itanium < %s | FileCheck %s --check-prefixes=CHECK,WIN64
; RUN: llc -O0 -mtriple=x86_64-windows-ntposix < %s | FileCheck %s --check-prefixes=CHECK,SYSV

; A 32-bit load through address space 280 is a fault probe: it is never folded
; into another instruction, and it is a call site of its own whose landing
; zeroes its register and resumes after it. The site's one catch clause names
; the function itself. Windows Itanium's table leads with its scope table.

declare i32 @__gxx_personality_seh0(...)
declare void @use(i32)

; CHECK-LABEL: f:
; CHECK:       .seh_handler __gxx_personality_seh0, @unwind, @except
; CHECK:       [[BEGIN:.Ltmp[0-9]+]]: # EH_LABEL
; SYSV-NEXT:   movl (%rdi), %eax
; WIN64-NEXT:  movl (%rcx), %eax
; CHECK-NEXT:  [[END:.Ltmp[0-9]+]]:
; CHECK-NEXT:  [[RESUME:.LBB0_[0-9]+]]:
; SYSV-NEXT:   addl %esi, %eax
; WIN64-NEXT:  addl %edx, %eax
; CHECK:       .seh_handlerdata
; WIN64:       .byte 1 # Scope table version
; CHECK:       GCC_except_table0:
; CHECK:       .byte 27 # @LPStart Encoding = pcrel sdata4
; CHECK-NEXT:  [[LPSTART:.Llpstart[0-9]+]]:
; CHECK-NEXT:  .long f.cold-[[LPSTART]]
; CHECK-NEXT:  .byte 155 # @TType Encoding = indirect pcrel sdata4
; CHECK:       .uleb128 [[BEGIN]]-.Lfunc_begin0 # >> Call Site 1 <<
; CHECK-NEXT:  .uleb128 [[END]]-[[BEGIN]]
; CHECK-NEXT:  .uleb128 [[LANDING:.Ltmp[0-9]+]]-f.cold
; CHECK-NEXT:  .byte 1 # On action: 1
; CHECK:       .byte 1 # >> Action Record 1 <<
; CHECK-NEXT:  # Catch TypeInfo 1
; CHECK-NEXT:  .byte 0 # No further actions
; CHECK:       [[TI:.Ltmp[0-9]+]]: # TypeInfo 1
; CHECK-NEXT:  .long __imp_f-[[TI]]
; CHECK:       [[LANDING]]: # EH_LABEL
; CHECK-NEXT:  movl $0, %eax
; CHECK-NEXT:  jmp [[RESUME]]
define i32 @f(ptr addrspace(280) %p, i32 %x) personality ptr @__gxx_personality_seh0 {
  %v = load volatile i32, ptr addrspace(280) %p
  %a = add i32 %v, %x
  ret i32 %a
}

; The prologue is not shrink-wrapped past a probe, whose landing is found
; through the frame's unwind information. The landing leaves the flags.
; CHECK-LABEL: g:
; CHECK:       .seh_proc g
; CHECK:       .seh_endprologue
; CHECK:       # EH_LABEL
; CHECK-NEXT:  movl
define void @g(i64 %p) personality ptr @__gxx_personality_seh0 {
entry:
  %q = inttoptr i64 %p to ptr addrspace(280)
  %v = load volatile i32, ptr addrspace(280) %q
  %t = and i32 %v, 48
  %c = icmp eq i32 %t, 0
  br i1 %c, label %cold, label %hot
hot:
  call void @use(i32 %v)
  ret void
cold:
  ret void
}

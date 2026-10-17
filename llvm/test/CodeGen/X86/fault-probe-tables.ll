; RUN: llc -O2 -verify-machineinstrs -mtriple=x86_64-pc-windows-ntposix < %s | FileCheck %s --check-prefixes=CHECK,X64
; RUN: llc -O2 -verify-machineinstrs -mtriple=aarch64-pc-windows-ntposix -mattr=+lse < %s | FileCheck %s --check-prefixes=CHECK,A64

; Each fault probe is a call site of its function's exception table spanning
; its one instruction, never merged with a neighbour's, whose landing pad's
; one catch clause names the function itself; under ehcontguard every landing
; is an EH continuation target, a cleanup funclet's own included.

declare i32 @rust_eh_personality(...)
declare void @may_throw()
declare void @drop()

define i64 @two(ptr %p, ptr %r) personality ptr @rust_eh_personality {
  %q = addrspacecast ptr %p to ptr addrspace(280)
  %s = addrspacecast ptr %r to ptr addrspace(280)
  %a = load atomic i64, ptr addrspace(280) %q acquire, align 8
  %b = load atomic i64, ptr addrspace(280) %s acquire, align 8
  %c = add i64 %a, %b
  ret i64 %c
}

; CHECK-LABEL: two:
; CHECK:       [[B1:\.Ltmp[0-9]+]]: {{[#/]+}} EH_LABEL
; CHECK-NEXT:  {{movq|ldar}}
; CHECK-NEXT:  [[E1:\.Ltmp[0-9]+]]:
; CHECK:       [[B2:\.Ltmp[0-9]+]]: {{[#/]+}} EH_LABEL
; CHECK-NEXT:  {{movq|ldar}}
; CHECK-NEXT:  [[E2:\.Ltmp[0-9]+]]:
; CHECK:       [[G1:\$ehgcr_[0-9_]+]]:
; CHECK-NEXT:  [[L1:\.Ltmp[0-9]+]]: {{[#/]+}} EH_LABEL
; CHECK:       [[G2:\$ehgcr_[0-9_]+]]:
; CHECK-NEXT:  [[L2:\.Ltmp[0-9]+]]: {{[#/]+}} EH_LABEL
; CHECK:       .uleb128 [[B1]]-.Lfunc_begin0 {{[#/]+}} >> Call Site 1 <<
; CHECK-NEXT:  .uleb128 [[E1]]-[[B1]]
; CHECK-NEXT:  .uleb128 [[L1]]-.Lfunc_begin0
; CHECK-NEXT:  .byte 1 {{[#/]+}} On action: 1
; CHECK-NEXT:  .uleb128 [[B2]]-.Lfunc_begin0 {{[#/]+}} >> Call Site 2 <<
; CHECK-NEXT:  .uleb128 [[E2]]-[[B2]]
; CHECK-NEXT:  .uleb128 [[L2]]-.Lfunc_begin0
; CHECK-NEXT:  .byte 1 {{[#/]+}} On action: 1
; CHECK-NEXT:  .Lcst_end0:
; X64:         .quad two {{[#/]+}} TypeInfo 1
; A64:         .xword two {{[#/]+}} TypeInfo 1

define i64 @in_cleanup(ptr %p) personality ptr @rust_eh_personality {
entry:
  invoke void @may_throw() to label %cont unwind label %pad
cont:
  %q = addrspacecast ptr %p to ptr addrspace(280)
  %v = load atomic i64, ptr addrspace(280) %q acquire, align 8
  ret i64 %v
pad:
  %cp = cleanuppad within none [i8 0]
  %q2 = addrspacecast ptr %p to ptr addrspace(280)
  %w = load atomic i64, ptr addrspace(280) %q2 acquire, align 8
  call void @drop() [ "funclet"(token %cp) ]
  cleanupret from %cp unwind to caller
}

; The parent's landing stays in the parent; the funclet's is laid out in the
; funclet.
; CHECK-LABEL: in_cleanup:
; CHECK:       [[PG:\$ehgcr_[0-9_]+]]:
; CHECK-NEXT:  {{.*}} EH_LABEL
; CHECK:       .seh_endproc
; CHECK:       .seh_proc "?dtor$
; CHECK:       [[FG:\$ehgcr_[0-9_]+]]:
; CHECK-NEXT:  {{.*}} EH_LABEL
; CHECK:       .seh_endproc

; CHECK:       .section .gehcont$y
; CHECK-NEXT:  .symidx [[G1]]
; CHECK-NEXT:  .symidx [[G2]]
; CHECK:       .symidx [[PG]]
; CHECK:       .symidx [[FG]]

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"ehcontguard", i32 1}

; RUN: llc -O2 -verify-machineinstrs -mtriple=aarch64-pc-windows-ntposix < %s | FileCheck %s
; RUN: llc -O0 -verify-machineinstrs -mtriple=aarch64-pc-windows-ntposix < %s | FileCheck %s

; The AArch64 twin of the x86-64 test: a cleanup is a funclet the unwinder
; calls with its frame's pointer in x1, which the funclet's own prologue
; copies into x29 to reach that frame's locals, and a cleanupret to another
; cleanup funclet continues there after the epilogue with x1 restored.

declare void @may_throw()
declare void @drop_a() nounwind
declare void @drop_b() nounwind
declare i32 @rust_eh_personality(...)

; CHECK-LABEL: chained:
; CHECK: .seh_handler rust_eh_personality, @unwind, @except
; CHECK: [[BEGIN1:.Ltmp[0-9]+]]:
; CHECK-NEXT: bl may_throw
; CHECK: [[END1:.Ltmp[0-9]+]]:
; CHECK: [[BEGIN2:.Ltmp[0-9]+]]:
; CHECK-NEXT: bl may_throw
; CHECK: [[END2:.Ltmp[0-9]+]]:
; CHECK: .seh_handlerdata
; CHECK-NEXT: .p2align 2
; CHECK-NEXT: .byte 1
; CHECK-NEXT: .uleb128 .Lcleanup_end{{[0-9]+}}-[[CB:.Lcleanup_begin[0-9]+]]
; CHECK-NEXT: [[CB]]:
; CHECK-NEXT: .uleb128 [[BEGIN1]]-[[FUNC:.Lfunc_begin[0-9]+]]
; CHECK-NEXT: .uleb128 [[FA:.LBB0_[0-9]+]]-[[FUNC]]
; CHECK-NEXT: .uleb128 [[BEGIN2]]-[[FUNC]]
; CHECK-NEXT: .uleb128 [[FB:.LBB0_[0-9]+]]-[[FUNC]]
; CHECK: GCC_except_table0:
; CHECK: .uleb128 [[BEGIN1]]-[[FUNC]]
; CHECK-NEXT: .uleb128 [[END1]]-[[BEGIN1]]
; CHECK-NEXT: .byte 0
; CHECK-NEXT: .byte 0
; CHECK: .uleb128 [[BEGIN2]]-[[FUNC]]
; CHECK-NEXT: .uleb128 [[END2]]-[[BEGIN2]]
; CHECK-NEXT: .byte 0
; CHECK-NEXT: .byte 0
; CHECK: .seh_endproc
; CHECK: [[FA]]:
; CHECK: mov x29, x1
; CHECK: bl drop_a
; CHECK: ret
; CHECK: [[FB]]:
; CHECK: mov x29, x1
; CHECK: bl drop_b
; CHECK: mov x1, x29
; CHECK-NEXT: b [[FA]]
define void @chained() personality ptr @rust_eh_personality {
entry:
  invoke void @may_throw() to label %mid unwind label %cleanup_a
mid:
  invoke void @may_throw() to label %done unwind label %cleanup_b
done:
  ret void
cleanup_a:
  %a = cleanuppad within none []
  call void @drop_a() [ "funclet"(token %a) ]
  cleanupret from %a unwind to caller
cleanup_b:
  %b = cleanuppad within none []
  call void @drop_b() [ "funclet"(token %b) ]
  cleanupret from %b unwind label %cleanup_a
}

; CHECK-LABEL: mixed:
; CHECK: $ehgcr_1_{{[0-9]+}}:
; CHECK: .seh_handlerdata
; CHECK-NEXT: .p2align 2
; CHECK-NEXT: .byte 1
; CHECK-NEXT: .uleb128 .Lcleanup_end{{[0-9]+}}-[[MCB:.Lcleanup_begin[0-9]+]]
; CHECK-NEXT: [[MCB]]:
; CHECK-NEXT: .uleb128 [[MBEGIN:.Ltmp[0-9]+]]-[[MFUNC:.Lfunc_begin[0-9]+]]
; CHECK-NEXT: .uleb128 .LBB1_{{[0-9]+}}-[[MFUNC]]
define void @mixed() personality ptr @rust_eh_personality {
entry:
  invoke void @may_throw() to label %mid unwind label %cleanup
mid:
  invoke void @may_throw() to label %done unwind label %catch
done:
  ret void
cleanup:
  %c = cleanuppad within none []
  call void @drop_a() [ "funclet"(token %c) ]
  cleanupret from %c unwind to caller
catch:
  %e = landingpad { ptr, i32 } catch ptr null
  ret void
}

; CHECK: .section .gehcont$y
; CHECK-NEXT: .symidx $ehgcr_1_{{[0-9]+}}
; CHECK-NOT: .symidx

!llvm.module.flags = !{!0}
!0 = !{i32 1, !"ehcontguard", i32 1}

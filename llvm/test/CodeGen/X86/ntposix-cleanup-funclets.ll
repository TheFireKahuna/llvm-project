; RUN: llc -O2 -verify-machineinstrs -mtriple=x86_64-pc-windows-ntposix < %s | FileCheck %s
; RUN: llc -O0 -verify-machineinstrs -mtriple=x86_64-pc-windows-ntposix < %s | FileCheck %s

; On NT-POSIX a cleanup is a funclet the unwinder calls with the establisher
; frame, and a catch is a landing pad it lands. The function's handler data
; is the cleanup table, then the LSDA, whose site for a called funclet names
; no pad. A cleanupret to another cleanup funclet continues there after the
; epilogue, with the establisher back in rdx.

declare void @may_throw()
declare void @drop_a() nounwind
declare void @drop_b() nounwind
declare i32 @rust_eh_personality(...)

; CHECK-LABEL: chained:
; CHECK: .seh_handler rust_eh_personality, @unwind, @except
; CHECK: [[BEGIN1:.Ltmp[0-9]+]]:
; CHECK-NEXT: callq may_throw
; CHECK: [[END1:.Ltmp[0-9]+]]:
; CHECK: [[BEGIN2:.Ltmp[0-9]+]]:
; CHECK-NEXT: callq may_throw
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
; CHECK: movq %rdx, 16(%rsp)
; CHECK: callq drop_a
; CHECK: retq
; CHECK: [[FB]]:
; CHECK: movq %rdx, 16(%rsp)
; CHECK: callq drop_b
; CHECK: movq 16(%rsp), %rdx
; CHECK-NEXT: jmp [[FA]]
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

; A catch in the same function is landed: its pad is in the LSDA and in the
; EH continuation table; the funclet is in neither.
; CHECK-LABEL: mixed:
; CHECK: [[MBEGIN1:.Ltmp[0-9]+]]:
; CHECK-NEXT: callq may_throw
; CHECK: [[MEND1:.Ltmp[0-9]+]]:
; CHECK: [[MBEGIN2:.Ltmp[0-9]+]]:
; CHECK-NEXT: callq may_throw
; CHECK: [[MEND2:.Ltmp[0-9]+]]:
; CHECK: $ehgcr_1_{{[0-9]+}}:
; CHECK: .seh_handlerdata
; CHECK-NEXT: .p2align 2
; CHECK-NEXT: .byte 1
; CHECK-NEXT: .uleb128 .Lcleanup_end{{[0-9]+}}-.Lcleanup_begin{{[0-9]+}}
; CHECK-NEXT: .Lcleanup_begin{{[0-9]+}}:
; CHECK-NEXT: .uleb128 [[MBEGIN1]]-[[MFUNC:.Lfunc_begin[0-9]+]]
; CHECK-NEXT: .uleb128 [[MF:.LBB1_[0-9]+]]-[[MFUNC]]
; CHECK: GCC_except_table1:
; CHECK: .uleb128 [[MBEGIN1]]-[[MFUNC]]
; CHECK-NEXT: .uleb128 [[MEND1]]-[[MBEGIN1]]
; CHECK-NEXT: .byte 0
; CHECK-NEXT: .byte 0
; CHECK: .uleb128 [[MBEGIN2]]-[[MFUNC]]
; CHECK-NEXT: .uleb128 [[MEND2]]-[[MBEGIN2]]
; CHECK-NEXT: .uleb128 {{.*}}-[[MFUNC]]
; CHECK-NEXT: .byte 1
; CHECK: [[MF]]:
; CHECK: callq drop_a
; CHECK: retq
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

; A terminating funclet, marked by its one true argument: its site names no
; pad and carries the empty filter, the action nothing passes.
; CHECK-LABEL: aborts:
; CHECK: [[ABEGIN:.Ltmp[0-9]+]]:
; CHECK-NEXT: callq may_throw
; CHECK: .seh_handlerdata
; CHECK-NEXT: .p2align 2
; CHECK-NEXT: .byte 1
; CHECK-NEXT: .uleb128 .Lcleanup_end{{[0-9]+}}-.Lcleanup_begin{{[0-9]+}}
; CHECK-NEXT: .Lcleanup_begin{{[0-9]+}}:
; CHECK-NEXT: .uleb128 [[ABEGIN]]-[[AFUNC:.Lfunc_begin[0-9]+]]
; CHECK-NEXT: .uleb128 [[AF:.LBB2_[0-9]+]]-[[AFUNC]]
; CHECK: GCC_except_table2:
; CHECK: .uleb128 [[ABEGIN]]-[[AFUNC]]
; CHECK-NEXT: .uleb128 {{.*}}-[[ABEGIN]]
; CHECK-NEXT: .byte 0
; CHECK-NEXT: .byte 1
; CHECK: .byte 127
; CHECK: .byte 0
; CHECK: [[AF]]:
; CHECK: callq abort
define void @aborts() personality ptr @rust_eh_personality {
entry:
  invoke void @may_throw() to label %done unwind label %terminate
done:
  ret void
terminate:
  %t = cleanuppad within none [i1 true]
  call void @abort() [ "funclet"(token %t) ]
  unreachable
}

declare void @abort() nounwind

; CHECK: .section .gehcont$y
; CHECK-NEXT: .symidx $ehgcr_1_{{[0-9]+}}
; CHECK-NOT: .symidx

!llvm.module.flags = !{!0}
!0 = !{i32 1, !"ehcontguard", i32 1}

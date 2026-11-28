; RUN: llc -mtriple=aarch64-unknown-windows-itanium -verify-machineinstrs < %s | FileCheck %s
; RUN: llc -mtriple=aarch64-unknown-windows-itanium -filetype=obj < %s -o %t.obj
; RUN: llvm-objdump -dr --no-show-raw-insn %t.obj | FileCheck %s --check-prefix=OBJ
; RUN: llvm-readobj --unwind %t.obj | FileCheck %s --check-prefix=UNWIND

; An overridable default begins with the check of its word, ahead of its
; unwind range, which with the exception tables begins past it. Where the word
; holds another function, the tail after the function compares the target's
; type prefix with the default's and branches to the CFG tail-call thunk with
; the target in X9; the own entry repeats the check and enters the body past
; it, or branches through the word with the type checked.

declare void @may_throw()
declare i32 @__gxx_personality_seh0(...)

; CHECK-LABEL: {{^}}leaf:
; CHECK-NEXT:    adrp x9, __imp_leaf
; CHECK-NEXT:    ldr x9, [x9, :lo12:__imp_leaf]
; CHECK-NEXT:    adr x16, leaf
; CHECK-NEXT:    cmp x9, x16
; CHECK-NEXT:    b.ne [[TAIL:.Lovr_tail[0-9]+]]
; CHECK-NEXT:  [[BODY:.Lovr_body[0-9]+]]:
; CHECK-NEXT:  // %bb.0:
; CHECK-NEXT:    add x0, x0, #1
; CHECK-NEXT:    ret
; CHECK:       [[TAIL]]:
; CHECK-NEXT:    ldur x17, [x16, #-8]
; CHECK-NEXT:    ldur x16, [x9, #-8]
; CHECK-NEXT:    cmp x16, x17
; CHECK-NEXT:    b.ne [[TRAP:.Lovr_trap[0-9]+]]
; CHECK-NEXT:    b __llvm_guard_tailcall
; CHECK-NEXT:  [[TRAP]]:
; CHECK-NEXT:    brk #0x8229
; CHECK-NEXT:    .p2align 4
; CHECK-NEXT:    .weak __ovr_own$leaf
; CHECK-NEXT:  __ovr_own$leaf:
; CHECK-NEXT:    adrp x9, __imp_leaf
; CHECK-NEXT:    ldr x9, [x9, :lo12:__imp_leaf]
; CHECK-NEXT:    adr x16, leaf
; CHECK-NEXT:    cmp x9, x16
; CHECK-NEXT:    b.eq [[BODY]]
; CHECK-NEXT:    ldur x17, [x16, #-8]
; CHECK-NEXT:    ldur x16, [x9, #-8]
; CHECK-NEXT:    cmp x16, x17
; CHECK-NEXT:    b.ne [[TRAP]]
; CHECK-NEXT:    br x9

; OBJ:      b.ne 0x2c
; OBJ:      2c: ldur x17, [x16, #-0x8]
; OBJ-NEXT:     ldur x16, [x9, #-0x8]
; OBJ-NEXT:     cmp x16, x17
; OBJ-NEXT:     b.ne
; OBJ-NEXT:     IMAGE_REL_ARM64_BRANCH19 [[TRAP:.+]]
; OBJ-NEXT:     b 0x3c
; OBJ-NEXT:     IMAGE_REL_LLVM_BRANCH_TAIL __llvm_guard_tailcall
; OBJ:      <[[TRAP]]>:
; OBJ-NEXT: 40: brk #0x8229
; OBJ:      <__llvm_guard_tailcall>:
define weak i64 @leaf(i64 %x) nounwind !kcfi_type !1 !overridable !2 {
  %r = add i64 %x, 1
  ret i64 %r
}

; CHECK-LABEL: {{^}}frame:
; CHECK:         b.ne [[TAIL:.Lovr_tail[0-9]+]]
; CHECK-NEXT:  [[BODY:.Lovr_body[0-9]+]]:
; CHECK-NEXT:  .seh_proc frame
; CHECK:       [[SITE:.Ltmp[0-9]+]]: // EH_LABEL
; CHECK-NEXT:    bl may_throw
; CHECK:         .uleb128 [[SITE]]-[[BODY]] // >> Call Site 1 <<
; CHECK:       [[TAIL]]:
; CHECK-NEXT:    ldur x17, [x16, #-8]
;; frame is at 0x90, and its unwind range begins past the 20-byte check.
; UNWIND:      Function: {{.*}} (0xA4)
; UNWIND-NEXT: ExceptionRecord:
; UNWIND-NEXT: ExceptionData {
; UNWIND-NEXT:   FunctionLength: 16
define weak void @frame() personality ptr @__gxx_personality_seh0 !kcfi_type !1 !overridable !3 {
  invoke void @may_throw() to label %done unwind label %cs
cs:
  %s = catchswitch within none [label %catch] unwind to caller
catch:
  %cp = catchpad within %s [ptr null]
  catchret from %cp to label %done
done:
  ret void
}

; CHECK-LABEL: {{^}}untyped:
; CHECK:       .Lovr_tail{{[0-9]+}}:
; CHECK-NEXT:    b __llvm_guard_tailcall
; CHECK:       __ovr_own$untyped:
; CHECK:         b.eq .Lovr_body{{[0-9]+}}
; CHECK-NEXT:    br x9
define weak void @untyped() nounwind !overridable !4 {
  ret void
}

!llvm.module.flags = !{!6, !7, !8}
!1 = !{i32 286331153}
!2 = !{i32 0}
!3 = !{i32 3, i32 2, i32 1}
!4 = !{i32 8}
!6 = !{i32 4, !"kcfi", i32 1}
!7 = !{i32 1, !"function-type-prefix", i32 119298566}
!8 = !{i32 2, !"cfguard", i32 2}

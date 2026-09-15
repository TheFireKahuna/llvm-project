; RUN: split-file %s %t
; RUN: not opt -passes=verify -disable-output %t/call.ll 2>&1 | FileCheck %s --check-prefix=CALL
; RUN: not opt -passes=verify -disable-output %t/targets.ll 2>&1 | FileCheck %s --check-prefix=TARGETS

;--- call.ll
declare void @llvm.experimental.nt.recovery(ptr)
define void @invalid(ptr %buffer) {
  call void @llvm.experimental.nt.recovery(ptr %buffer)
  ret void
}
; CALL: NT recovery must use callbr

;--- targets.ll
declare void @llvm.experimental.nt.recovery(ptr)
define void @invalid(ptr %buffer) {
  callbr void @llvm.experimental.nt.recovery(ptr %buffer)
      to label %normal [label %one, label %two]
normal:
  ret void
one:
  ret void
two:
  ret void
}
; TARGETS: NT recovery requires exactly one recovery successor

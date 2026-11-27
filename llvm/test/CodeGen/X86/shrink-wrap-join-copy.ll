; RUN: llc -mtriple=x86_64-unknown-linux-gnu -verify-machineinstrs < %s | FileCheck %s
; RUN: llc -mtriple=x86_64-unknown-windows-itanium -verify-machineinstrs < %s | FileCheck %s --check-prefixes=CHECK,WIN

; The count is kept in a callee-saved register across the loop's calls, and
; the empty path sets it only for the join to copy it into the return
; register. Shrink-wrapping splits the restore point and that path sets the
; return register itself, as one 32-bit definition that defines all 64 bits,
; outside the frame.

declare i16 @put(ptr, i16)

define i64 @count(ptr %o, ptr %s, i64 %n) {
; CHECK-LABEL: count:
; CHECK-NOT:     push
; CHECK:         jle [[EXIT:.LBB0_[0-9]+]]
; WIN-NEXT:    .Lunwind_begin0:
; WIN-NEXT:    .seh_proc count
; CHECK:         pushq
; WIN:           .seh_endproc
; CHECK:       [[EXIT]]:
; CHECK-NEXT:    xorl %eax, %eax
; CHECK-NEXT:    retq
entry:
  %c = icmp sgt i64 %n, 0
  br i1 %c, label %loop, label %exit
loop:
  %i = phi i64 [ 0, %entry ], [ %i1, %inc ]
  %p = getelementptr i16, ptr %s, i64 %i
  %v = load i16, ptr %p
  %r = call i16 @put(ptr %o, i16 %v)
  %e = icmp eq i16 %r, -1
  br i1 %e, label %exit, label %inc
inc:
  %i1 = add nuw nsw i64 %i, 1
  %d = icmp eq i64 %i1, %n
  br i1 %d, label %exit, label %loop
exit:
  %ret = phi i64 [ 0, %entry ], [ %i, %loop ], [ %n, %inc ]
  ret i64 %ret
}

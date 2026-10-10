; RUN: llc -mtriple=x86_64-pc-windows-ntposix < %s | FileCheck %s

; NT-POSIX uses the Win64 prologue, which realigns the stack only after
; establishing the frame, so the C++ EH tables name the unwind help slot, a
; fixed object, by its offset from that frame, as on any Windows x64 target.

declare void @may_throw()
declare i32 @__CxxFrameHandler3(...)

define void @realigned() personality ptr @__CxxFrameHandler3 {
; CHECK-LABEL: realigned:
; CHECK:       subq $104, %rsp
; CHECK:       leaq 96(%rsp), %rbp
; CHECK:       andq $-64, %rsp
; CHECK:       movq $-2, (%rbp)
; CHECK-LABEL: $cppxdata$realigned:
; CHECK:       .long 96 # UnwindHelp
entry:
  %buf = alloca [4 x i64], align 64
  store volatile i64 0, ptr %buf
  invoke void @may_throw()
          to label %done unwind label %dispatch

dispatch:
  %cs = catchswitch within none [label %catch] unwind to caller

catch:
  %cp = catchpad within %cs [ptr null, i32 64, ptr null]
  catchret from %cp to label %done

done:
  ret void
}

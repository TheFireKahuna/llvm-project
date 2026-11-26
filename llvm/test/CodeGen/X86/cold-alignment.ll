; RUN: llc -mtriple=x86_64-unknown-linux-gnu < %s | FileCheck %s
; RUN: llc -mtriple=x86_64-unknown-windows-itanium < %s | FileCheck %s

; A function that is cold by its attribute is entered rarely, so neither its
; entry nor its loops are aligned beyond what the function requires. A function
; that Control Flow Guard may reach keeps the 16 bytes its entry requires.

; CHECK-NOT:  .p2align
; CHECK-NOT:  .prefalign
; CHECK:      local:
; CHECK-NOT:  .p2align
; CHECK:      callq ext
; CHECK-NOT:  .p2align
; CHECK:      .p2align 4
; CHECK-NOT:  .p2align
; CHECK:      visible:
; CHECK-NOT:  .p2align
; CHECK:      callq ext
; CHECK-NOT:  .p2align
; CHECK:      .p2align 4
; CHECK-NOT:  .p2align
; CHECK:      hot:
; CHECK:      .p2align 4
; CHECK:      callq ext

declare void @ext(i32)

define internal void @local(i32 %n) cold noinline {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %next, %loop ]
  call void @ext(i32 %i)
  %next = add i32 %i, 1
  %c = icmp ult i32 %next, %n
  br i1 %c, label %loop, label %exit
exit:
  ret void
}

define void @visible(i32 %n) cold {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %next, %loop ]
  call void @ext(i32 %i)
  %next = add i32 %i, 1
  %c = icmp ult i32 %next, %n
  br i1 %c, label %loop, label %exit
exit:
  ret void
}

define void @hot(i32 %n) {
entry:
  call void @local(i32 %n)
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %next, %loop ]
  call void @ext(i32 %i)
  %next = add i32 %i, 1
  %c = icmp ult i32 %next, %n
  br i1 %c, label %loop, label %exit
exit:
  ret void
}

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"cfguard", i32 2}

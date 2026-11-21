; RUN: llc -mtriple=x86_64-unknown-linux-gnu < %s | FileCheck %s --check-prefixes=CHECK,ELF
; RUN: llc -mtriple=x86_64-unknown-windows-itanium < %s | FileCheck %s --check-prefixes=CHECK,WI

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

; On x86-64 Windows Itanium a loop's alignment is 16 bytes within 10 and 8
; otherwise, and each directive names the loop's end, after its last jump
; back, by which the assembler pads a loop that a cache line can keep only to
; keep it within one. A loop that is fallen into states its end alone.
; CHECK:      hot:
; ELF:        .p2align 4
; WI:         .p2align 4, , 10, [[END:.Lloop_end[0-9]+]]
; WI-NEXT:    .p2align 3, , , [[END]]
; CHECK:      callq ext
; WI:         jb
; WI-NEXT:    [[END]]:
; CHECK:      fallen:
; ELF-NOT:    .p2align
; WI:         .p2align 0, , , [[END:.Lloop_end[0-9]+]]
; WI:         jb
; WI-NEXT:    [[END]]:
; ELF:        retq

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

define void @fallen(ptr %p, i32 %n) {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %next, %loop ]
  %q = getelementptr i32, ptr %p, i32 %i
  store i32 %i, ptr %q
  %next = add i32 %i, 1
  %c = icmp ult i32 %next, %n
  br i1 %c, label %loop, label %exit, !prof !1
exit:
  ret void
}

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"cfguard", i32 2}
!1 = !{!"branch_weights", i32 1, i32 1}

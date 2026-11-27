; RUN: llc -o - %s -mtriple=aarch64-windows -aarch64-min-jump-table-entries=4 -aarch64-enable-compress-jump-tables=0 | FileCheck %s
; RUN: llc -o - %s -mtriple=aarch64-windows -aarch64-min-jump-table-entries=4 -aarch64-enable-compress-jump-tables=0 -filetype=obj | llvm-readobj --unwind - | FileCheck %s -check-prefix=UNWIND
; RUN: llc -o - %s -mtriple=aarch64-unknown-uefi -aarch64-min-jump-table-entries=4 -aarch64-enable-compress-jump-tables=0 | FileCheck %s

define void @f(i32 %x) {
entry:
  switch i32 %x, label %sw.epilog [
    i32 0, label %sw.bb
    i32 1, label %sw.bb1
    i32 2, label %sw.bb2
    i32 3, label %sw.bb3
  ]

sw.bb:
  tail call void @g(i32 0, i32 4)
  br label %sw.epilog

sw.bb1:
  tail call void @g(i32 1, i32 5)
  br label %sw.epilog

sw.bb2:
  tail call void @g(i32 2, i32 6)
  br label %sw.epilog

sw.bb3:
  tail call void @g(i32 3, i32 7)
  br label %sw.epilog

sw.epilog:
  tail call void @g(i32 10, i32 8)
  ret void
}

declare void @g(i32, i32)

; The prologue is shrink-wrapped below the range check, so the unwind range
; begins there, and ends after the epilogue, which branches to the code
; outside it.
; CHECK:    .text
; CHECK:    f:
; CHECK:    b.hi .LBB0_7
; CHECK:    .seh_proc f
; CHECK:    bl g
; CHECK:    .seh_endepilogue
; CHECK-NEXT: b .LBB0_7
; CHECK-NEXT: .seh_endfunclet
; CHECK-NEXT: .seh_endproc
; CHECK-NEXT: .LBB0_7:
; CHECK:    b g
; CHECK-NEXT:  .section .rdata,"dr"
; CHECK-NEXT: .p2align  2
; CHECK-NEXT: .LJTI0_0:
; CHECK:    .word .LBB0_2-.Ltmp0
; CHECK:    .word .LBB0_5-.Ltmp0
; CHECK:    .word .LBB0_3-.Ltmp0
; CHECK:    .word .LBB0_4-.Ltmp0

; Check that we can emit an object file with correct unwind info.
; UNWIND: FunctionLength: {{[1-9][0-9]*}}

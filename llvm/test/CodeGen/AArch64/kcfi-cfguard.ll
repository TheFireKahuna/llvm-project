; RUN: llc -mtriple=aarch64-pc-windows-msvc -verify-machineinstrs < %s | FileCheck %s

;; On AArch64 the Control Flow Guard check mechanism leaves the kcfi operand
;; bundle on the call to the real target, so the KCFI check tests that target,
;; for an invoke as for a call.

; CHECK-LABEL: f1:
; CHECK:         mov x15, x0
; CHECK:         blr x8
; CHECK:         ldur w16, [x0, #-4]
; CHECK:         cmp w16, w17
; CHECK-NEXT:    b.eq .Ltmp[[#PASS:]]
; CHECK-NEXT:    brk #0x8220
; CHECK-NEXT:  .Ltmp[[#PASS]]:
; CHECK-NEXT:    blr x0
define void @f1(ptr noundef %x) {
  call void %x() [ "kcfi"(i32 12345678) ]
  ret void
}

declare i32 @__CxxFrameHandler3(...)

; CHECK-LABEL: f2:
; CHECK:         mov x15, x0
; CHECK:         blr x8
; CHECK:         ldur w16, [x0, #-4]
; CHECK:         cmp w16, w17
; CHECK-NEXT:    b.eq .Ltmp[[#PASS:]]
; CHECK-NEXT:    brk #0x8220
; CHECK-NEXT:  .Ltmp[[#PASS]]:
; CHECK-NEXT:    blr x0
define void @f2(ptr noundef %x) personality ptr @__CxxFrameHandler3 {
  invoke void %x() [ "kcfi"(i32 12345678) ]
    to label %cont
    unwind label %cleanup
cont:
  ret void
cleanup:
  %pad = cleanuppad within none []
  cleanupret from %pad unwind to caller
}

!llvm.module.flags = !{!0, !1}
!0 = !{i32 4, !"kcfi", i32 1}
!1 = !{i32 2, !"cfguard", i32 2}

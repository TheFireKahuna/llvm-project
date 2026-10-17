; RUN: llc -mtriple=aarch64-unknown-windows-itanium < %s | FileCheck %s
; RUN: llc -mtriple=aarch64-unknown-windows-itanium < %s | \
; RUN:   FileCheck %s --check-prefix=CHECKER

;; A virtual call whose vtable pointer load carries vtable_guard metadata is
;; routed through the class's guard thunk with the vtable pointer in the Guard
;; dispatch register x9. The thunk frames itself, tests the pointer against the
;; run [start, end) and the stride mask word, and branches through the slot on
;; a hit. On a miss it loads the slot into x15 and calls the type's KCFI check
;; thunk, which validates it and returns, then branches through it.

; CHECK-LABEL: call:
; CHECK:         ldr x9, [x0]
; CHECK:         bl __llvm_vcall_1A_8

; CHECK:       __llvm_vcall_1A_8:
; CHECK:         stp x29, x30, [sp, #-16]!
; CHECK:         adrp x16, __llvm_vtables$1A
; CHECK:         add x16, x16, :lo12:__llvm_vtables$1A
; CHECK:         cmp x9, x16
; CHECK:         b.lo [[MISS:\.L.*]]
; CHECK:         sub x17, x9, x16
; CHECK:         adrp x16, __llvm_vtables_end$1A
; CHECK:         cmp x9, x16
; CHECK:         b.hs [[MISS]]
; CHECK:         adrp x16, __llvm_vtables_mask$1A
; CHECK:         ldr w16, [x16, :lo12:__llvm_vtables_mask$1A]
; CHECK:         tst w17, w16
; CHECK:         b.ne [[MISS]]
; CHECK:         ldr x16, [x9, #8]
; CHECK:         ldp x29, x30, [sp], #16
; CHECK:         br x16
; CHECK:       [[MISS]]:
; CHECK:         ldr x15, [x9, #8]
; CHECK:         bl __llvm_kcfi_check_0001e240
; CHECK:         ldp x29, x30, [sp], #16
; CHECK:         br x15

;; A class whose run another image may own too tests the run of the image
;; that owns it before the slow path: x16 and x17 take its start and end, then
;; its third word less its start, the stride's mask.
; CHECK:       __llvm_vcall_1B_8:
; CHECK:         tst w17, w16
; CHECK-NEXT:    b.ne [[OWNER:\.L.*]]
; CHECK:         br x16
; CHECK:       [[OWNER]]:
; CHECK-NEXT:    adrp x16, __llvm_vtables_owner$1B
; CHECK-NEXT:    add x16, x16, :lo12:__llvm_vtables_owner$1B
; CHECK-NEXT:    ldp x16, x17, [x16]
; CHECK-NEXT:    sub x17, x17, x16
; CHECK-NEXT:    sub x16, x9, x16
; CHECK-NEXT:    cmp x16, x17
; CHECK-NEXT:    b.hs [[MISS2:\.L.*]]
; CHECK-NEXT:    adrp x17, __llvm_vtables_owner$1B+16
; CHECK-NEXT:    ldr x17, [x17, :lo12:__llvm_vtables_owner$1B+16]
; CHECK-NEXT:    sub x17, x17, x9
; CHECK-NEXT:    add x17, x17, x16
; CHECK-NEXT:    tst x16, x17
; CHECK-NEXT:    b.ne [[MISS2]]
; CHECK-NEXT:    ldr x16, [x9, #8]
; CHECK:         br x16
; CHECK:       [[MISS2]]:
; CHECK-NEXT:    ldr x15, [x9, #8]
; CHECK-NEXT:    bl __llvm_kcfi_check_0001e240

;; The check thunk the miss path calls is emitted.
; CHECKER: __llvm_kcfi_check_0001e240:

define void @call(ptr %a) !kcfi_type !3 {
entry:
  %vtable = load ptr, ptr %a
  %slot = getelementptr inbounds i8, ptr %vtable, i64 8
  %f = load ptr, ptr %slot, !vtable_guard !4
  call void %f(ptr %a) [ "kcfi"(i32 123456) ]
  ret void
}

define void @owned(ptr %a) !kcfi_type !3 {
entry:
  %vtable = load ptr, ptr %a
  %slot = getelementptr inbounds i8, ptr %vtable, i64 8
  %f = load ptr, ptr %slot, !vtable_guard !5
  call void %f(ptr %a) [ "kcfi"(i32 123456) ]
  ret void
}

!llvm.module.flags = !{!0, !1, !2}
!0 = !{i32 4, !"kcfi", i32 1}
!1 = !{i32 4, !"function-type-prefix", i32 -559038737}
!2 = !{i32 1, !"MaxTLSAlign", i32 65536}
!3 = !{i32 123456}
!4 = !{!"1A", i64 8}
!5 = !{!"1B", i64 8, i1 true}

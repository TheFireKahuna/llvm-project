; RUN: split-file %s %t
; RUN: llc -mtriple=aarch64-unknown-windows-itanium < %t/a.ll | FileCheck %s
; RUN: llc -mtriple=aarch64-unknown-windows-itanium -filetype=obj < %t/a.ll \
; RUN:   | llvm-objdump -d - | FileCheck %s --check-prefix=BYTES
; RUN: llc -mtriple=aarch64-unknown-windows-itanium < %t/prefix.ll \
; RUN:   | FileCheck %s --check-prefix=PREFIX

;; With the kcfi-marker module flag, llvm.kcfi.check at offset 16 calls the
;; vfn check thunk of its type with the target in X15. The thunk compares the
;; 8 bytes at 16 bytes before the target, the second type that a function
;; which can occupy a vtable slot carries and the first four bytes of the
;; marker, and otherwise has the check thunk's form, with the check thunk's
;; mismatch routine. Its page test allows for a prefix of 16 bytes, and a
;; patchable-function prefix moves the compare and widens the test.

; CHECK-LABEL: virtual:
; CHECK:         mov x15, x0
; CHECK-NEXT:    bl __llvm_kcfi_vfn_check_89abcdef
; CHECK-NEXT:    blr x0

; CHECK:       .weak __llvm_kcfi_check_mismatch_89abcdef
; CHECK-NEXT:  __llvm_kcfi_check_mismatch_89abcdef = __llvm_kcfi_trap
; CHECK-NEXT:  .section .text,"xr",discard,__llvm_kcfi_vfn_check_89abcdef
; CHECK:       __llvm_kcfi_vfn_check_89abcdef:
; CHECK-NEXT:    adrp x16, __llvm_code_start
; CHECK-NEXT:    add x16, x16, :lo12:__llvm_code_start
; CHECK-NEXT:    cmp x15, x16
; CHECK-NEXT:    b.lo [[OUT:.Ltmp[0-9]+]]
; CHECK-NEXT:    adrp x16, __llvm_code_end
; CHECK-NEXT:    add x16, x16, :lo12:__llvm_code_end
; CHECK-NEXT:    cmp x15, x16
; CHECK-NEXT:    b.hs [[OUT]]
; CHECK-NEXT:    ldur x16, [x15, #-16]
; CHECK-NEXT:    mov x17, #52719
; CHECK-NEXT:    movk x17, #35243, lsl #16
; CHECK-NEXT:    movk x17, #7951, lsl #32
; CHECK-NEXT:    movk x17, #61312, lsl #48
; CHECK-NEXT:    cmp x16, x17
; CHECK-NEXT:    b.ne __llvm_kcfi_check_mismatch_89abcdef
; CHECK-NEXT:    ret
; CHECK-NEXT:  [[OUT]]:
; CHECK-NEXT:    tst x15, #0xff0
; CHECK-NEXT:    b.eq __llvm_kcfi_check_mismatch_89abcdef
; CHECK-NEXT:    ldur x16, [x15, #-16]
; CHECK-NEXT:    mov x17, #52719
; CHECK-NEXT:    movk x17, #35243, lsl #16
; CHECK-NEXT:    movk x17, #7951, lsl #32
; CHECK-NEXT:    movk x17, #61312, lsl #48
; CHECK-NEXT:    cmp x16, x17
; CHECK-NEXT:    b.ne __llvm_kcfi_check_mismatch_89abcdef
; CHECK-NEXT:    adrp x16, __guard_check_icall_fptr
; CHECK-NEXT:    ldr x16, [x16, :lo12:__guard_check_icall_fptr]
; CHECK-NEXT:    br x16

; BYTES-LABEL: <__llvm_kcfi_vfn_check_89abcdef>:
; BYTES:       20: f85f01f0 ldur x16, [x15, #-0x10]
; BYTES:       40: f27c1dff tst x15, #0xff0

; PREFIX-LABEL: __llvm_kcfi_vfn_check_89abcdef:
; PREFIX:         ldur x16, [x15, #-32]
; PREFIX:         tst x15, #0xfe0
; PREFIX-NEXT:    b.eq __llvm_kcfi_check_mismatch_89abcdef
; PREFIX-NEXT:    ldur x16, [x15, #-32]

;--- a.ll
define void @virtual(ptr %p) {
  call void @llvm.kcfi.check(ptr %p, i32 -1985229329, i32 16)
  call void %p() #0
  ret void
}

attributes #0 = { "guard_nocf" }

!llvm.module.flags = !{!0, !1}
!0 = !{i32 4, !"kcfi", i32 1}
!1 = !{i32 4, !"kcfi-marker", i32 -559038737}

;--- prefix.ll
define void @virtual(ptr %p) {
  call void @llvm.kcfi.check(ptr %p, i32 -1985229329, i32 16)
  call void %p() #0
  ret void
}

attributes #0 = { "guard_nocf" }

!llvm.module.flags = !{!0, !1, !2}
!0 = !{i32 4, !"kcfi", i32 1}
!1 = !{i32 4, !"kcfi-marker", i32 -559038737}
!2 = !{i32 4, !"kcfi-offset", i32 4}

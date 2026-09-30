; RUN: llc -mtriple=aarch64-unknown-windows-itanium < %s | FileCheck %s
; RUN: llc -mtriple=aarch64-unknown-windows-itanium -global-isel < %s \
; RUN:   | FileCheck %s --check-prefix=CALL

;; With the kcfi-marker module flag, a call with a KCFI type goes through a
;; per-type check thunk, called with the target in X15 as the guard check
;; function is, a COMDAT that compares the 8 bytes before the target, the end
;; of the marker, the byte after it and the type, and on a mismatch continues
;; into a weak alias of the routine that fails fast if the target carries the
;; marker. On a match, it returns for a target inside the code range, whose
;; bounds are weak aliases of one byte in a COMDAT, and continues into the guard check
;; function for any other.
;; The module has no cfguard flag, as under -mguard=none, and the thunks are
;; the same as with one. A call marked kcfi_local, whose every target is in
;; the image, goes through a local thunk, which fails fast for a target outside
;; the range unless the range is empty, as it is in an image that was not
;; sealed, where the guard check function decides.

; CHECK-LABEL: f1:
; CALL-LABEL:  f1:
; CHECK:         mov x15, x0
; CHECK-NEXT:    bl __llvm_kcfi_check_12345678
; CHECK-NEXT:    blr x0
; CALL:          bl __llvm_kcfi_check_12345678
; CALL-NEXT:     blr x{{[0-9]+}}
define void @f1(ptr %p) {
  call void %p() [ "kcfi"(i32 305419896) ]
  ret void
}

; CHECK-LABEL: f2:
; CHECK:         bl __llvm_kcfi_local_check_12345678
define void @f2(ptr %p) {
  call void %p() [ "kcfi"(i32 305419896) ], !kcfi_local !2
  ret void
}

; CHECK:       .weak __llvm_code_start
; CHECK-NEXT:  __llvm_code_start = __llvm_code_empty
; CHECK-NEXT:  .weak __llvm_code_end
; CHECK-NEXT:  __llvm_code_end = __llvm_code_empty
; CHECK-NEXT:  .section .rdata,"dr",discard,__llvm_code_empty
; CHECK-NEXT:  .globl __llvm_code_empty
; CHECK-NEXT:  __llvm_code_empty:
; CHECK-NEXT:  .byte 0
; CHECK-NEXT:  .weak __llvm_kcfi_check_mismatch_12345678
; CHECK-NEXT:  __llvm_kcfi_check_mismatch_12345678 = __llvm_kcfi_check_open
; CHECK-NEXT:  .section .text,"xr",discard,__llvm_kcfi_check_12345678
; CHECK:       .globl __llvm_kcfi_check_12345678
; CHECK-NEXT:  .p2align 4
; CHECK-NEXT:  __llvm_kcfi_check_12345678:
; CHECK-NEXT:    ldur x16, [x15, #-8]
; CHECK-NEXT:    mov x17, #44478
; CHECK-NEXT:    movk x17, #47326, lsl #16
; CHECK-NEXT:    movk x17, #22136, lsl #32
; CHECK-NEXT:    movk x17, #4660, lsl #48
; CHECK-NEXT:    cmp x16, x17
; CHECK-NEXT:    b.ne __llvm_kcfi_check_mismatch_12345678
; CHECK-NEXT:    adrp x16, __llvm_code_start
; CHECK-NEXT:    add x16, x16, :lo12:__llvm_code_start
; CHECK-NEXT:    cmp x15, x16
; CHECK-NEXT:    b.lo [[GUARD:.Ltmp[0-9]+]]
; CHECK-NEXT:    adrp x16, __llvm_code_end
; CHECK-NEXT:    add x16, x16, :lo12:__llvm_code_end
; CHECK-NEXT:    cmp x15, x16
; CHECK-NEXT:    b.hs [[GUARD]]
; CHECK-NEXT:    ret
; CHECK-NEXT:  [[GUARD]]:
; CHECK-NEXT:    adrp x16, __guard_check_icall_fptr
; CHECK-NEXT:    ldr x16, [x16, :lo12:__guard_check_icall_fptr]
; CHECK-NEXT:    br x16

;; The local thunk shares the type's mismatch routine and the open routine.
; CHECK:       .section .text,"xr",discard,__llvm_kcfi_local_check_12345678
; CHECK:       __llvm_kcfi_local_check_12345678:
; CHECK-NEXT:    ldur x16, [x15, #-8]
; CHECK-NEXT:    mov x17, #44478
; CHECK-NEXT:    movk x17, #47326, lsl #16
; CHECK-NEXT:    movk x17, #22136, lsl #32
; CHECK-NEXT:    movk x17, #4660, lsl #48
; CHECK-NEXT:    cmp x16, x17
; CHECK-NEXT:    b.ne __llvm_kcfi_check_mismatch_12345678
; CHECK-NEXT:    adrp x16, __llvm_code_start
; CHECK-NEXT:    add x16, x16, :lo12:__llvm_code_start
; CHECK-NEXT:    adrp x17, __llvm_code_end
; CHECK-NEXT:    add x17, x17, :lo12:__llvm_code_end
; CHECK-NEXT:    cmp x15, x16
; CHECK-NEXT:    b.lo [[GUARD:.Ltmp[0-9]+]]
; CHECK-NEXT:    cmp x15, x17
; CHECK-NEXT:    b.hs [[GUARD]]
; CHECK-NEXT:    ret
; CHECK-NEXT:  [[GUARD]]:
; CHECK-NEXT:    cmp x16, x17
; CHECK-NEXT:    b.ne [[TRAP:.Ltmp[0-9]+]]
; CHECK-NEXT:    adrp x16, __guard_check_icall_fptr
; CHECK-NEXT:    ldr x16, [x16, :lo12:__guard_check_icall_fptr]
; CHECK-NEXT:    br x16
; CHECK-NEXT:  [[TRAP]]:
; CHECK-NEXT:    mov w0, #64
; CHECK-NEXT:    brk #0xf003
; CHECK-NOT:   __llvm_kcfi_check_mismatch_12345678 =
; CHECK:       .section .text,"xr",discard,__llvm_kcfi_check_open
; CHECK:       .globl __llvm_kcfi_check_open
; CHECK-NEXT:  .p2align 4
; CHECK-NEXT:  __llvm_kcfi_check_open:
; CHECK-NEXT:    ldur x16, [x15, #-12]
; CHECK-NEXT:    mov x17, #7951
; CHECK-NEXT:    movk x17, #61312, lsl #16
; CHECK-NEXT:    movk x17, #44478, lsl #32
; CHECK-NEXT:    movk x17, #47326, lsl #48
; CHECK-NEXT:    cmp x16, x17
; CHECK-NEXT:    b.eq [[TRAP:.Ltmp[0-9]+]]
; CHECK-NEXT:    adrp x16, __guard_check_icall_fptr
; CHECK-NEXT:    ldr x16, [x16, :lo12:__guard_check_icall_fptr]
; CHECK-NEXT:    br x16
; CHECK-NEXT:  [[TRAP]]:
; CHECK-NEXT:    mov w0, #64
; CHECK-NEXT:    brk #0xf003

!llvm.module.flags = !{!0, !1}
!0 = !{i32 4, !"kcfi", i32 1}
!1 = !{i32 4, !"kcfi-marker", i32 -559038737}
!2 = !{}

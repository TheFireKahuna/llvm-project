; RUN: llc -mtriple=aarch64-unknown-windows-itanium < %s | FileCheck %s
; RUN: llc -mtriple=aarch64-unknown-windows-itanium -filetype=obj < %s \
; RUN:   | llvm-objdump -d - | FileCheck %s --check-prefix=BYTES

;; An object with a KCFI check thunk emits the trap and the check scanners,
;; each in a COMDAT, for open routines of any object to use. A scanner takes
;; the target in X15 and the type's list in X16, and uses only X17 besides.
;; It fails fast if the target carries the marker, which it reads only at page
;; offset 16 or more and compares 24 bits at a time, and then walks the list:
;; a zero word is skipped, an odd word ends it, and any other word is the
;; address of a cell, whose value, if it is the target, makes the check
;; return. On reaching the end, the static scanner fails fast and the dynamic
;; one continues into the guard check function.

; CHECK-LABEL: __llvm_kcfi_check_open:
; CHECK-NEXT:    tst x15, #0xff0
; CHECK-NEXT:    b.eq [[WALK:.Ltmp[0-9]+]]
; CHECK-NEXT:    ldur x17, [x15, #-12]
; CHECK-NEXT:    sub x17, x17, #3855
; CHECK-NEXT:    sub x17, x17, #2049, lsl #12
; CHECK-NEXT:    tst x17, #0xffffff
; CHECK-NEXT:    b.ne [[WALK]]
; CHECK-NEXT:    lsr x17, x17, #24
; CHECK-NEXT:    sub x17, x17, #3823
; CHECK-NEXT:    sub x17, x17, #2779, lsl #12
; CHECK-NEXT:    tst x17, #0xffffff
; CHECK-NEXT:    b.ne [[WALK]]
; CHECK-NEXT:    lsr x17, x17, #24
; CHECK-NEXT:    sub x17, x17, #2270
; CHECK-NEXT:    sub x17, x17, #11, lsl #12
; CHECK-NEXT:    cbz x17, [[TRAP:.Ltmp[0-9]+]]
; CHECK-NEXT:  [[WALK]]:
; CHECK-NEXT:    ldr x17, [x16], #8
; CHECK-NEXT:    cbz x17, [[WALK]]
; CHECK-NEXT:    tbnz w17, #0, [[TRAP]]
; CHECK-NEXT:    ldr x17, [x17]
; CHECK-NEXT:    cmp x17, x15
; CHECK-NEXT:    b.ne [[WALK]]
; CHECK-NEXT:    ret
; CHECK-NEXT:  [[TRAP]]:
; CHECK-NEXT:    mov w0, #64
; CHECK-NEXT:    brk #0xf003

; CHECK-LABEL: __llvm_kcfi_check_open_dynamic:
; CHECK-NEXT:    tst x15, #0xff0
; CHECK-NEXT:    b.eq [[WALK:.Ltmp[0-9]+]]
; CHECK-NEXT:    ldur x17, [x15, #-12]
; CHECK-COUNT-2: sub x17, x17,
; CHECK-NEXT:    tst x17, #0xffffff
; CHECK-NEXT:    b.ne [[WALK]]
; CHECK-NEXT:    lsr x17, x17, #24
; CHECK-COUNT-2: sub x17, x17,
; CHECK-NEXT:    tst x17, #0xffffff
; CHECK-NEXT:    b.ne [[WALK]]
; CHECK-NEXT:    lsr x17, x17, #24
; CHECK-COUNT-2: sub x17, x17,
; CHECK-NEXT:    cbz x17, [[TRAP:.Ltmp[0-9]+]]
; CHECK-NEXT:  [[WALK]]:
; CHECK-NEXT:    ldr x17, [x16], #8
; CHECK-NEXT:    cbz x17, [[WALK]]
; CHECK-NEXT:    tbnz w17, #0, [[MISS:.Ltmp[0-9]+]]
; CHECK-NEXT:    ldr x17, [x17]
; CHECK-NEXT:    cmp x17, x15
; CHECK-NEXT:    b.ne [[WALK]]
; CHECK-NEXT:    ret
; CHECK-NEXT:  [[MISS]]:
; CHECK-NEXT:    adrp x16, __guard_check_icall_fptr
; CHECK-NEXT:    ldr x16, [x16, :lo12:__guard_check_icall_fptr]
; CHECK-NEXT:    br x16
; CHECK-NEXT:  [[TRAP]]:
; CHECK-NEXT:    mov w0, #64
; CHECK-NEXT:    brk #0xf003

; CHECK:       .section .text,"xr",discard,__llvm_kcfi_trap
; CHECK-LABEL: __llvm_kcfi_trap:
; CHECK-NEXT:    mov w0, #64
; CHECK-NEXT:    brk #0xf003

; BYTES-LABEL: <__llvm_kcfi_check_open>:
; BYTES-NEXT:    0: f27c1dff tst x15, #0xff0
; BYTES-NEXT:    4: 540001e0 b.eq 0x40
; BYTES-NEXT:    8: f85f41f1 ldur x17, [x15, #-0xc]
; BYTES-NEXT:    c: d13c3e31 sub x17, x17, #0xf0f
; BYTES-NEXT:   10: d1600631 sub x17, x17, #0x801, lsl #12
; BYTES-NEXT:   14: f2405e3f tst x17, #0xffffff
; BYTES-NEXT:   18: 54000141 b.ne 0x40
; BYTES-NEXT:   1c: d358fe31 lsr x17, x17, #24
; BYTES-NEXT:   20: d13bbe31 sub x17, x17, #0xeef
; BYTES-NEXT:   24: d16b6e31 sub x17, x17, #0xadb, lsl #12
; BYTES-NEXT:   28: f2405e3f tst x17, #0xffffff
; BYTES-NEXT:   2c: 540000a1 b.ne 0x40
; BYTES-NEXT:   30: d358fe31 lsr x17, x17, #24
; BYTES-NEXT:   34: d1237a31 sub x17, x17, #0x8de
; BYTES-NEXT:   38: d1402e31 sub x17, x17, #0xb, lsl #12
; BYTES-NEXT:   3c: b4000111 cbz x17, 0x5c
; BYTES-NEXT:   40: f8408611 ldr x17, [x16], #0x8
; BYTES-NEXT:   44: b4fffff1 cbz x17, 0x40
; BYTES-NEXT:   48: 370000b1 tbnz w17, #0x0, 0x5c
; BYTES-NEXT:   4c: f9400231 ldr x17, [x17]
; BYTES-NEXT:   50: eb0f023f cmp x17, x15
; BYTES-NEXT:   54: 54ffff61 b.ne 0x40
; BYTES-NEXT:   58: d65f03c0 ret
; BYTES-NEXT:   5c: 52800800 mov w0, #0x40
; BYTES-NEXT:   60: d43e0060 brk #0xf003
; BYTES-EMPTY:
; BYTES-LABEL: <__llvm_kcfi_trap>:
; BYTES-NEXT:    0: 52800800 mov w0, #0x40
; BYTES-NEXT:    4: d43e0060 brk #0xf003
; BYTES-EMPTY:

define void @f1(ptr %p) {
  call void %p() [ "kcfi"(i32 305419896) ]
  ret void
}

!llvm.module.flags = !{!0, !1}
!0 = !{i32 4, !"kcfi", i32 1}
!1 = !{i32 4, !"function-type-prefix", i32 -559038737}
;; Opening a type dynamically makes the module define the scanners, which its
;; open routines pass the type's list to.
!kcfi.dynamic = !{!2}
!2 = !{i32 1122867}

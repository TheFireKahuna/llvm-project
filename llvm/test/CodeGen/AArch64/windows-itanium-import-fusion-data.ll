; RUN: llc < %s -mtriple=aarch64-unknown-windows-itanium | FileCheck %s
; RUN: llc < %s -mtriple=aarch64-pc-windows-ntposix | FileCheck %s
; RUN: llc < %s -mtriple=aarch64-unknown-windows-itanium -verify-machineinstrs \
; RUN:   -o /dev/null
; RUN: llc < %s -mtriple=aarch64-unknown-windows-itanium -O0 | \
; RUN:   FileCheck %s --check-prefix=NONE
; RUN: llc < %s -mtriple=aarch64-pc-windows-msvc | \
; RUN:   FileCheck %s --check-prefix=NONE

; A load of the import pointer of data into the register that holds the
; pointer's page, where only accesses at an unsigned offset from the register
; read it, is described with each of them, so that the linker can give each
; access the data's offset in its page and make the load dead where the data
; is in the image. One whose register only an add of a constant then reads is
; described with the add, which the linker can fold into the load there.

; NONE-NOT: .linkfused

$iv = comdat any
@iv = linkonce_odr global [8 x i32] zeroinitializer, comdat, align 4
@ext = external global i32
@arr = external global [64 x i64]
@slot = external global ptr

; A pair of accesses is one access.
define i32 @access() {
; CHECK-LABEL: access:
; CHECK:         adrp x8, __imp_ext
; CHECK-NEXT:  [[L0:\.Lfused[0-9]+]]:
; CHECK-NEXT:    .linkfusedread
; CHECK-NEXT:    ldr x8, [x8, :lo12:__imp_ext]
; CHECK-NEXT:    .linkfusedaccess [[L0]], x8
; CHECK-NEXT:    ldp w9, w8, [x8]
  %a = load i32, ptr @ext
  %p = getelementptr i8, ptr @ext, i64 4
  %b = load i32, ptr %p
  %s = add i32 %a, %b
  ret i32 %s
}

; A store through the register is an access; a store of it is not.
define void @store(i64 %v) {
; CHECK-LABEL: store:
; CHECK:       [[L1:\.Lfused[0-9]+]]:
; CHECK-NEXT:    .linkfusedread
; CHECK-NEXT:    ldr x8, [x8, :lo12:__imp_arr]
; CHECK-NEXT:    .linkfusedaccess [[L1]], x8
; CHECK-NEXT:    str x0, [x8, #16]
  store i64 %v, ptr getelementptr (i8, ptr @arr, i64 16)
  ret void
}

define void @storeself() {
; CHECK-LABEL: storeself:
; CHECK-NOT:     .linkfused
; CHECK:         ret
  store ptr @slot, ptr @slot
  ret void
}

; The register's only reader adds a constant to it.
define ptr @add() {
; CHECK-LABEL: add:
; CHECK:         adrp x8, __imp_iv
; CHECK-NEXT:  [[L2:\.Lfused[0-9]+]]:
; CHECK-NEXT:    ldr x8, [x8, :lo12:__imp_iv]
; CHECK-NEXT:    .linkfusedadd [[L2]], x8
; CHECK-NEXT:    add x0, x8, #12
  ret ptr getelementptr (i8, ptr @iv, i64 12)
}

; An access with a register offset is not described, nor so the load.
define i64 @indexed(i64 %i) {
; CHECK-LABEL: indexed:
; CHECK-NOT:     .linkfused
; CHECK:         ret
  %p = getelementptr i64, ptr @arr, i64 %i
  %v = load i64, ptr %p
  ret i64 %v
}

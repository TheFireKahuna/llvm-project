; RUN: llc < %s -mtriple=aarch64-unknown-windows-itanium | FileCheck %s
; RUN: llc < %s -mtriple=aarch64-unknown-windows-itanium \
; RUN:   -verify-machineinstrs -o /dev/null
; RUN: llc < %s -mtriple=aarch64-pc-windows-msvc | \
; RUN:   FileCheck %s --check-prefix=NONE

; A load of the import pointer of data whose pointer the register of an
; earlier load of the import pointer of other data still holds, on every path
; to it, is described with that load, so that the linker can make it an add of
; the distance between the data where both are in one other image. A register
; written in between, or a call's, is no such register.

; NONE-NOT: .linkanchor

@a = external global i32
@b = external global i32
declare void @take(ptr, ptr)
declare void @f()

define void @pair() {
; CHECK-LABEL: pair:
; CHECK:       [[A:\.Lfused[0-9]+]]:
; CHECK-NEXT:    ldr x0, [x0, :lo12:__imp_a]
; CHECK-NEXT:    .linkanchor [[A]], x0
; CHECK-NEXT:    ldr x1, [x1, :lo12:__imp_b]
  call void @take(ptr @a, ptr @b)
  ret void
}

define void @call() {
; CHECK-LABEL: call:
; CHECK-NOT:   .linkanchor
; CHECK:       ret
  store volatile i32 0, ptr @a
  call void @f()
  store volatile i32 0, ptr @b
  ret void
}

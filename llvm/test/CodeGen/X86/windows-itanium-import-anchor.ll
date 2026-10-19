; RUN: llc < %s -mtriple=x86_64-unknown-windows-itanium | FileCheck %s
; RUN: llc < %s -mtriple=x86_64-unknown-windows-itanium -verify-machineinstrs \
; RUN:   -o /dev/null
; RUN: llc < %s -mtriple=x86_64-pc-windows-msvc | \
; RUN:   FileCheck %s --check-prefix=NONE

; A load of the import pointer of data whose pointer the register of an
; earlier load of the import pointer of other data still holds, on every path
; to it, is described with that load, so that the linker can make it an lea of
; the distance between the data where both are in one other image. A register
; written in between, or a call's, is no such register, nor is the register
; of a load of a function's pointer.

; NONE-NOT: .linkanchor

@a = external global i32
@b = external global i32
@c = external global i32
declare void @take(ptr, ptr)
declare void @take3(ptr, ptr, ptr)
declare void @f()
declare dllimport void @imp()

define void @pair() {
; CHECK-LABEL: pair:
; CHECK:       [[A:\.Lfused[0-9]+]]:
; CHECK-NEXT:    movq __imp_a(%rip), %rcx
; CHECK-NEXT:    .linkanchor [[A]], %rcx
; CHECK-NEXT:    movq __imp_b(%rip), %rdx
  call void @take(ptr @a, ptr @b)
  ret void
}

; Each later load may take its pointer from each earlier register.
define void @three() {
; CHECK-LABEL: three:
; CHECK:       [[A:\.Lfused[0-9]+]]:
; CHECK-NEXT:    movq __imp_a(%rip), %rcx
; CHECK-NEXT:    .linkanchor [[A]], %rcx
; CHECK-NEXT:  [[B:\.Lfused[0-9]+]]:
; CHECK-NEXT:    movq __imp_b(%rip), %rdx
; CHECK-NEXT:    .linkanchor [[B]], %rdx
; CHECK-NEXT:    .linkanchor [[A]], %rcx
; CHECK-NEXT:    movq __imp_c(%rip), %r8
  call void @take3(ptr @a, ptr @b, ptr @c)
  ret void
}

; The register is written before the second load.
define i32 @clobbered() {
; CHECK-LABEL: clobbered:
; CHECK-NOT:   .linkanchor
; CHECK:       retq
  %x = load i32, ptr @a
  %y = load i32, ptr @b
  %s = add i32 %x, %y
  ret i32 %s
}

; A call writes the register.
define void @call() {
; CHECK-LABEL: call:
; CHECK-NOT:   .linkanchor
; CHECK:       retq
  store volatile i32 0, ptr @a
  call void @f()
  store volatile i32 0, ptr @b
  ret void
}

; A function's pointer gives none.
define void @function() {
; CHECK-LABEL: function:
; CHECK-NOT:   .linkanchor
; CHECK:       retq
  call void @take(ptr @imp, ptr @b)
  ret void
}

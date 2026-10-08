; RUN: llc < %s -mtriple=aarch64-unknown-windows-itanium | FileCheck %s
; RUN: llc < %s -mtriple=aarch64-pc-windows-ntposix | FileCheck %s
; RUN: llc < %s -mtriple=aarch64-pc-windows-msvc | FileCheck %s --check-prefix=MSVC

; On Windows Itanium and NT-POSIX the address of a function the module does
; not define is loaded from its import pointer, even when the declaration is
; dso_local and calls to it are direct, and the pointer is listed in the
; address-taken import table. That holds for an extern_weak function too,
; which the object keeps a weak external, so that the linker binds the pointer
; to zero when the function is absent. A hidden or defined function's address
; is computed.

declare dso_local void @f()
declare hidden void @hidden()
declare extern_weak void @weak()
declare void @use(ptr)

define void @defined() {
  ret void
}

define void @take() {
; CHECK-LABEL: take:
; CHECK:         bl f
; CHECK:         adrp x0, __imp_f
; CHECK-NEXT:    ldr x0, [x0, :lo12:__imp_f]
; CHECK-NEXT:    bl use
; CHECK:         adrp x0, hidden
; CHECK-NEXT:    add x0, x0, :lo12:hidden
; CHECK-NEXT:    bl use
; CHECK:         adrp x0, __imp_weak
; CHECK-NEXT:    ldr x0, [x0, :lo12:__imp_weak]
; CHECK-NEXT:    bl use
; CHECK:         adrp x0, defined
; CHECK-NEXT:    add x0, x0, :lo12:defined
; CHECK-NEXT:    bl use
; MSVC-LABEL:  take:
; MSVC:          bl f
; MSVC:          adrp x0, f
; MSVC-NEXT:     add x0, x0, :lo12:f
  call void @f()
  call void @use(ptr @f)
  call void @use(ptr @hidden)
  call void @use(ptr @weak)
  call void @use(ptr @defined)
  ret void
}

; CHECK:      .section .giats$y
; CHECK-NEXT: .symidx __imp_f
; CHECK-NEXT: .symidx __imp_weak
; CHECK:      .weak weak
; MSVC-NOT:   __imp_f

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"cfguard", i32 2}

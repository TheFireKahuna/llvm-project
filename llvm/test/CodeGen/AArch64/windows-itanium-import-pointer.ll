; RUN: llc < %s -mtriple=aarch64-unknown-windows-itanium | FileCheck %s
; RUN: llc < %s -mtriple=aarch64-pc-windows-ntposix | FileCheck %s
; RUN: llc < %s -mtriple=aarch64-pc-windows-msvc | FileCheck %s --check-prefix=MSVC

; On Windows Itanium and NT-POSIX a variable that is not dso_local may be
; provided by another image, whether it is declared or defined weakly, and is
; reached through its import pointer rather than a stub. So is an extern_weak
; variable, which the object keeps a weak external, so that the linker binds
; the pointer to zero when the variable is absent.

$comdatvar = comdat any

@var = external global i32
@dsolocalvar = external dso_local global i32
@weakvar = extern_weak global i32
@comdatvar = linkonce_odr global i32 1, comdat
@strongvar = global i32 2

define i32 @getVar() {
; CHECK-LABEL: getVar:
; CHECK:         adrp x8, __imp_var
; CHECK-NEXT:    ldr x8, [x8, :lo12:__imp_var]
; CHECK-NEXT:    ldr w0, [x8]
; MSVC-LABEL:  getVar:
; MSVC:          adrp x8, var
; MSVC-NEXT:     ldr w0, [x8, :lo12:var]
  %v = load i32, ptr @var
  ret i32 %v
}

define i32 @getDsoLocalVar() {
; CHECK-LABEL: getDsoLocalVar:
; CHECK:         adrp x8, dsolocalvar
; CHECK-NEXT:    ldr w0, [x8, :lo12:dsolocalvar]
  %v = load i32, ptr @dsolocalvar
  ret i32 %v
}

define i32 @getWeakVar() {
; CHECK-LABEL: getWeakVar:
; CHECK:         adrp x8, __imp_weakvar
; CHECK-NEXT:    ldr x8, [x8, :lo12:__imp_weakvar]
  %v = load i32, ptr @weakvar
  ret i32 %v
}

define i32 @getComdatVar() {
; CHECK-LABEL: getComdatVar:
; CHECK:         adrp x8, __imp_comdatvar
; CHECK-NEXT:    ldr x8, [x8, :lo12:__imp_comdatvar]
; CHECK-NEXT:    ldr w0, [x8]
; MSVC-LABEL:  getComdatVar:
; MSVC:          adrp x8, comdatvar
; MSVC-NEXT:     ldr w0, [x8, :lo12:comdatvar]
  %v = load i32, ptr @comdatvar
  ret i32 %v
}

define i32 @getStrongVar() {
; CHECK-LABEL: getStrongVar:
; CHECK:         adrp x8, strongvar
; CHECK-NEXT:    ldr w0, [x8, :lo12:strongvar]
  %v = load i32, ptr @strongvar
  ret i32 %v
}

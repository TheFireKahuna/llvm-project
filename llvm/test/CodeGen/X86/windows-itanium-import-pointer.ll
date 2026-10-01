; RUN: llc < %s -mtriple=x86_64-unknown-windows-itanium | FileCheck %s
; RUN: llc < %s -mtriple=x86_64-pc-windows-ntposix | FileCheck %s
; RUN: llc < %s -mtriple=x86_64-pc-windows-msvc | FileCheck %s --check-prefix=MSVC

; On Windows Itanium and NT-POSIX a variable that is not dso_local may be
; provided by another image, whether it is declared or defined weakly, and is
; reached through its import pointer rather than a stub. An extern_weak
; variable keeps the stub, since it may resolve to zero.

$comdatvar = comdat any

@var = external global i32
@dsolocalvar = external dso_local global i32
@weakvar = extern_weak global i32
@comdatvar = linkonce_odr global i32 1, comdat
@strongvar = global i32 2
@tlsvar = external thread_local global i32

define i32 @getVar() {
; CHECK-LABEL: getVar:
; CHECK:         movq __imp_var(%rip), %rax
; CHECK-NEXT:    movl (%rax), %eax
; MSVC-LABEL:  getVar:
; MSVC:          movl var(%rip), %eax
  %v = load i32, ptr @var
  ret i32 %v
}

define i32 @getDsoLocalVar() {
; CHECK-LABEL: getDsoLocalVar:
; CHECK:         movl dsolocalvar(%rip), %eax
  %v = load i32, ptr @dsolocalvar
  ret i32 %v
}

define i32 @getWeakVar() {
; CHECK-LABEL: getWeakVar:
; CHECK:         movq .refptr.weakvar(%rip), %rax
; CHECK-NEXT:    movl (%rax), %eax
  %v = load i32, ptr @weakvar
  ret i32 %v
}

define i32 @getComdatVar() {
; CHECK-LABEL: getComdatVar:
; CHECK:         movq __imp_comdatvar(%rip), %rax
; CHECK-NEXT:    movl (%rax), %eax
; MSVC-LABEL:  getComdatVar:
; MSVC:          movl comdatvar(%rip), %eax
  %v = load i32, ptr @comdatvar
  ret i32 %v
}

define i32 @getStrongVar() {
; CHECK-LABEL: getStrongVar:
; CHECK:         movl strongvar(%rip), %eax
  %v = load i32, ptr @strongvar
  ret i32 %v
}

define i32 @getTLSVar() {
; CHECK-LABEL: getTLSVar:
; CHECK:         movl tlsvar@SECREL32({{%r[a-z0-9]+}}), %eax
  %v = load i32, ptr @tlsvar
  ret i32 %v
}

; CHECK-NOT: .refptr.var
; CHECK-NOT: .refptr.comdatvar

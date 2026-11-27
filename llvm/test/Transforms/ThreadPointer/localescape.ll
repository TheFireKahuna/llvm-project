; RUN: opt -mtriple=x86_64-windows-itanium -passes=thread-pointer -S %s | FileCheck %s
; RUN: opt -mtriple=aarch64-windows-ntposix -passes=thread-pointer -S %s | FileCheck %s

; A derivation at the head of the entry block goes after the frame's escape,
; which stays there with the allocas.

@t = thread_local global i32 0

; CHECK-LABEL: define i32 @escapes()
; CHECK-NEXT:  entry:
; CHECK-NEXT:    %b = alloca i32
; CHECK-NEXT:    call void (...) @llvm.localescape(ptr %b)
; CHECK:         load atomic i64, ptr @"__imp___tp_map$val" acquire
define i32 @escapes() {
entry:
  %b = alloca i32
  call void (...) @llvm.localescape(ptr %b)
  %p = call ptr @llvm.threadlocal.address.p0(ptr @t)
  %v = load i32, ptr %p
  ret i32 %v
}

declare void @llvm.localescape(...)
declare ptr @llvm.threadlocal.address.p0(ptr)

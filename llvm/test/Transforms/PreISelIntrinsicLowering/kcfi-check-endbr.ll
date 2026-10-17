; RUN: opt -mtriple=x86_64-unknown-linux-gnu -passes=pre-isel-intrinsic-lowering -S < %s | FileCheck %s --check-prefix=X86
; RUN: opt -mtriple=aarch64-unknown-linux-gnu -passes=pre-isel-intrinsic-lowering -S < %s | FileCheck %s --check-prefix=A64

;; x86 stores a type that would spell ENDBR64 plus one, at the entry and as
;; the second type at offset 16, so the expansion compares those words with
;; that value. Other words, and other targets, compare the type as given.

; X86-LABEL: define void @f(
; X86:         icmp ne i32 %{{.*}}, -98693132
; X86:         icmp ne i32 %{{.*}}, -98693132
; X86:         icmp ne i32 %{{.*}}, -98693133
; A64-LABEL: define void @f(
; A64:         icmp ne i32 %{{.*}}, -98693133
; A64:         icmp ne i32 %{{.*}}, -98693133
; A64:         icmp ne i32 %{{.*}}, -98693133
define void @f(ptr %p) {
  call void @llvm.kcfi.check(ptr %p, i32 -98693133, i32 4)
  call void @llvm.kcfi.check(ptr %p, i32 -98693133, i32 16)
  call void @llvm.kcfi.check(ptr %p, i32 -98693133, i32 8)
  call void %p()
  ret void
}

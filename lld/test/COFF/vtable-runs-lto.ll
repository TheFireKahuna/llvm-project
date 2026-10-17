; REQUIRES: x86
;; A bitcode file's checks reference the vtable range symbols, which the
;; linker defines, so they are not reported as undefined before LTO; with no
;; run they are empty ranges, which nm does not list.

; RUN: llvm-as %s -o %t.obj
; RUN: lld-link -entry:main -subsystem:console %t.obj -out:%t.exe
; RUN: llvm-nm %t.exe | FileCheck %s --allow-empty
; CHECK-NOT: __llvm_vtables

target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-windows-itanium"

@"__llvm_vtables$1A" = external dso_local constant i8
@"__llvm_vtables_end$1A" = external dso_local constant i8
@"__llvm_vtables_mask$1A" = external dso_local constant i32
@"__llvm_vtables_owner$1A" = external dso_local constant [3 x ptr]

define i64 @main() {
  %m = load i32, ptr @"__llvm_vtables_mask$1A"
  %w = load ptr, ptr @"__llvm_vtables_owner$1A"
  %a = ptrtoint ptr @"__llvm_vtables$1A" to i64
  %b = ptrtoint ptr @"__llvm_vtables_end$1A" to i64
  %c = ptrtoint ptr %w to i64
  %d = zext i32 %m to i64
  %s1 = sub i64 %b, %a
  %s2 = add i64 %s1, %c
  %s3 = add i64 %s2, %d
  ret i64 %s3
}

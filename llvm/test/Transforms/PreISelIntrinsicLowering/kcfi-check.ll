; DEFINE: %{opt} = opt -passes=pre-isel-intrinsic-lowering
; RUN: %{opt} -mtriple=x86_64-unknown-windows-itanium -S < %s | FileCheck %s --check-prefixes=CHECK,X86
; RUN: %{opt} -mtriple=aarch64-pc-windows-ntposix -S < %s | FileCheck %s --check-prefixes=CHECK,AARCH64
; RUN: sed 's/^!llvm.module.flags = !{}/!llvm.module.flags = !{!0}/' %s \
; RUN:   | not %{opt} -mtriple=x86_64-unknown-windows-itanium -disable-output 2>&1 | FileCheck %s --check-prefix=PATCHABLE
; RUN: sed 's/^!llvm.module.flags = !{}/!llvm.module.flags = !{!1}/' %s \
; RUN:   | %{opt} -mtriple=x86_64-unknown-windows-itanium -S | FileCheck %s --check-prefix=PREFIX

;; llvm.kcfi.check becomes a load of the type word at the given distance before
;; the function's entry, a compare, and a trap.

declare void @llvm.kcfi.check(ptr, i32 immarg, i32 immarg)

define void @f(ptr %p) {
; CHECK-LABEL: define void @f(
; CHECK:         [[ADDR:%.*]] = getelementptr i8, ptr %p, i32 -16
; CHECK-NEXT:    [[WORD:%.*]] = load i32, ptr [[ADDR]], align 1
; CHECK-NEXT:    [[BAD:%.*]] = icmp ne i32 [[WORD]], 12345678
; CHECK-NEXT:    br i1 [[BAD]], label %[[TRAP:.*]], label %[[CONT:.*]], !prof
; CHECK:       [[TRAP]]:
; CHECK-NEXT:    call void @llvm.trap()
; CHECK-NEXT:    unreachable
; CHECK:       [[CONT]]:
; CHECK-NEXT:    call void %p()
; CHECK-NOT:   @llvm.kcfi.check(
  call void @llvm.kcfi.check(ptr %p, i32 12345678, i32 16)
  call void %p()
  ret void
}

;; Under a function type prefix a word before the type is the prefix's second
;; word, and the marker instruction after it states that the prefix has one:
;; the 8 bytes are compared as one, here with 0F 1F 81 and the marker's first
;; byte, so the check fails on a prefix without a second word whatever precedes
;; it. The type itself is compared alone.
; PREFIX-LABEL: define void @f(
; PREFIX:         [[ADDR:%.*]] = getelementptr i8, ptr %p, i32 -16
; PREFIX-NEXT:    [[WORD:%.*]] = load i64, ptr [[ADDR]], align 1
; PREFIX-NEXT:    icmp ne i64 [[WORD]], 1189265925899837774
; PREFIX-LABEL: define void @endbr(
; PREFIX:         load i32, ptr

;; x86 stores a type that would spell ENDBR64 plus one.
define void @endbr(ptr %p) {
; CHECK-LABEL: define void @endbr(
; CHECK:         getelementptr i8, ptr %p, i32 -4
; X86:           icmp ne i32 %{{.*}}, -98693132
; AARCH64:       icmp ne i32 %{{.*}}, -98693133
  call void @llvm.kcfi.check(ptr %p, i32 -98693133, i32 4)
  call void %p()
  ret void
}

; PATCHABLE: error: a patchable-function prefix is not compatible with llvm.kcfi.check on this target

!llvm.module.flags = !{}
!0 = !{i32 4, !"kcfi-offset", i32 4}
!1 = !{i32 1, !"function-type-prefix", i32 16}

; RUN: split-file %s %t
; RUN: opt -S -passes=cfguard %t/x64.ll | FileCheck %s
; RUN: opt -S -passes=cfguard %t/arm64.ll | FileCheck %s --check-prefix=ARM64

;; A test of membership tags, which LowerTypeTests leaves for a function type
;; it checks by membership, merges with the KCFI check of a call it dominates
;; into the type's member thunk, which takes a target that carries the call's
;; type and one of the tags, and continues into the type's ordinary thunk on a
;; miss. A test that guards no KCFI check becomes a call to a member check
;; thunk without a type, whose miss fails fast, or false if it lists no tag.

;--- x64.ll
target triple = "x86_64-unknown-windows-itanium"

; CHECK-LABEL: define i32 @call(
; CHECK-NOT:     llvm.kcfi.member.test
; CHECK:         br i1 true,
; CHECK:         call i32 @__llvm_kcfi_member_dispatch_12345678_0000abcd(i32 %x) [ "cfguardtarget"(ptr %p) ]{{$}}
define i32 @call(ptr %p, i32 %x) {
  %t = call i1 @llvm.kcfi.member.test(ptr %p, metadata i32 43981)
  br i1 %t, label %cont, label %trap
trap:
  call void @llvm.ubsantrap(i8 64)
  unreachable
cont:
  %r = call i32 %p(i32 %x) [ "kcfi"(i32 305419896) ]
  ret i32 %r
}

;; A type with several classes lists each class's tag in the thunk's name.
; CHECK-LABEL: define void @classes(
; CHECK:         call void @__llvm_kcfi_member_dispatch_12345678_0000abcd_00001234() [ "cfguardtarget"(ptr %p) ]{{$}}
define void @classes(ptr %p) {
  %t = call i1 @llvm.kcfi.member.test(ptr %p, metadata !{i32 43981, i32 4660})
  br i1 %t, label %cont, label %trap
trap:
  call void @llvm.ubsantrap(i8 64)
  unreachable
cont:
  call void %p() [ "kcfi"(i32 305419896) ]
  ret void
}

;; The non-virtual branch of a member function pointer call.
; CHECK-LABEL: define void @memptr(
; CHECK:         call cfguard_checkcc void @__llvm_kcfi_member_check_12345678_0000abcd(ptr %p)
define void @memptr(ptr %p) {
  %t = call i1 @llvm.kcfi.member.test(ptr %p, metadata i32 43981)
  br i1 %t, label %cont, label %trap
trap:
  call void @llvm.ubsantrap(i8 64)
  unreachable
cont:
  call void @llvm.kcfi.check(ptr %p, i32 305419896, i32 4)
  call void %p() "guard_nocf"
  ret void
}

; CHECK-LABEL: define void @unchecked(
; CHECK:         call cfguard_checkcc void @__llvm_kcfi_member_check_00000000_0000abcd(ptr %p)
; CHECK:         br i1 true,
define void @unchecked(ptr %p) {
  %t = call i1 @llvm.kcfi.member.test(ptr %p, metadata i32 43981)
  br i1 %t, label %cont, label %trap
trap:
  call void @llvm.ubsantrap(i8 64)
  unreachable
cont:
  call void %p() "guard_nocf"
  ret void
}

;; A call takes the tags of every test of its target that dominates it, and
;; only those. The thunk compares the call's type too, so a tag of another
;; type's class passes no target.
; CHECK-LABEL: define void @two(
; CHECK:         call void @__llvm_kcfi_member_dispatch_12345678_0000abcd() [ "cfguardtarget"(ptr %p) ]{{$}}
; CHECK:         call void @__llvm_kcfi_member_dispatch_11223344_0000abcd_00001234(double %d) [ "cfguardtarget"(ptr %p) ]{{$}}
; CHECK-NOT:     llvm.kcfi.member.test
define void @two(ptr %p, double %d) {
  %t1 = call i1 @llvm.kcfi.member.test(ptr %p, metadata i32 43981)
  br i1 %t1, label %cont1, label %trap
trap:
  call void @llvm.ubsantrap(i8 64)
  unreachable
cont1:
  call void %p() [ "kcfi"(i32 305419896) ]
  %t2 = call i1 @llvm.kcfi.member.test(ptr %p, metadata i32 4660)
  br i1 %t2, label %cont2, label %trap
cont2:
  call void %p(double %d) [ "kcfi"(i32 287454020) ]
  ret void
}

;; A test that does not dominate the call is checked in place.
; CHECK-LABEL: define void @apart(
; CHECK:         call cfguard_checkcc void @__llvm_kcfi_member_check_00000000_0000abcd(ptr %p)
; CHECK:       other:
; CHECK-NEXT:    call void @__llvm_kcfi_dispatch_12345678() [ "cfguardtarget"(ptr %p) ]{{$}}
define void @apart(ptr %p, i1 %c) {
  br i1 %c, label %tested, label %other
tested:
  %t = call i1 @llvm.kcfi.member.test(ptr %p, metadata i32 43981)
  br i1 %t, label %done, label %trap
trap:
  call void @llvm.ubsantrap(i8 64)
  unreachable
other:
  call void %p() [ "kcfi"(i32 305419896) ]
  br label %done
done:
  ret void
}

;; A test of no tags that guards a call keeps the member thunk, whose tags no
;; target matches; one that guards no call is false.
; CHECK-LABEL: define void @none(
; CHECK:         call void @__llvm_kcfi_member_dispatch_12345678() [ "cfguardtarget"(ptr %p) ]{{$}}
; CHECK-LABEL: define i1 @none_unchecked(
; CHECK-NEXT:    ret i1 false
define void @none(ptr %p) {
  %t = call i1 @llvm.kcfi.member.test(ptr %p, metadata !{})
  br i1 %t, label %cont, label %trap
trap:
  call void @llvm.ubsantrap(i8 64)
  unreachable
cont:
  call void %p() [ "kcfi"(i32 305419896) ]
  ret void
}
define i1 @none_unchecked(ptr %p) {
  %t = call i1 @llvm.kcfi.member.test(ptr %p, metadata !{})
  ret i1 %t
}

;; A call whose targets are all in the image takes the local member thunk,
;; whose miss falls back on the local thunk.
; CHECK-LABEL: define void @local(
; CHECK:         call void @__llvm_kcfi_member_local_dispatch_12345678_0000abcd() [ "cfguardtarget"(ptr %p) ]{{$}}
define void @local(ptr %p) {
  %t = call i1 @llvm.kcfi.member.test(ptr %p, metadata i32 43981)
  br i1 %t, label %cont, label %trap
trap:
  call void @llvm.ubsantrap(i8 64)
  unreachable
cont:
  call void %p() [ "kcfi"(i32 305419896) ], !kcfi_local !{}
  ret void
}

;; Each thunk carries its tags, and a member thunk's miss falls back on the
;; type's ordinary thunk, which the backend emits.
; CHECK: declare hidden void @__llvm_kcfi_dispatch_12345678()
; CHECK: declare !kcfi_member_tags [[ONE:![0-9]+]] hidden void @__llvm_kcfi_member_dispatch_12345678_0000abcd()
; CHECK: declare !kcfi_member_tags [[TWO:![0-9]+]] hidden void @__llvm_kcfi_member_dispatch_12345678_0000abcd_00001234()
; CHECK: declare hidden void @__llvm_kcfi_local_dispatch_12345678()
; CHECK: [[ONE]] = !{i32 43981}
; CHECK: [[TWO]] = !{i32 43981, i32 4660}

declare i1 @llvm.kcfi.member.test(ptr, metadata)
declare void @llvm.kcfi.check(ptr, i32, i32)
declare void @llvm.ubsantrap(i8)

!llvm.module.flags = !{!0, !1}
!0 = !{i32 4, !"kcfi", i32 1}
!1 = !{i32 4, !"kcfi-marker", i32 -559038737}

;--- arm64.ll
target triple = "aarch64-unknown-windows-itanium"

; ARM64-LABEL: define i32 @call(
; ARM64:         call cfguard_checkcc void @__llvm_kcfi_member_check_12345678_0000abcd(ptr %p)
; ARM64-NEXT:    call i32 %p(i32 %x){{$}}
define i32 @call(ptr %p, i32 %x) {
  %t = call i1 @llvm.kcfi.member.test(ptr %p, metadata i32 43981)
  br i1 %t, label %cont, label %trap
trap:
  call void @llvm.ubsantrap(i8 64)
  unreachable
cont:
  %r = call i32 %p(i32 %x) [ "kcfi"(i32 305419896) ]
  ret i32 %r
}

declare i1 @llvm.kcfi.member.test(ptr, metadata)
declare void @llvm.ubsantrap(i8)

!llvm.module.flags = !{!0, !1}
!0 = !{i32 4, !"kcfi", i32 1}
!1 = !{i32 4, !"kcfi-marker", i32 -559038737}

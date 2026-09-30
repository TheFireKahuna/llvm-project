; RUN: split-file %s %t
; RUN: opt -S -passes=cfguard %t/nocf.ll | FileCheck %s --check-prefixes=CHECK,NOCF
; RUN: opt -S -passes=cfguard %t/cf.ll | FileCheck %s --check-prefixes=CHECK,CF
; RUN: opt -S -passes=cfguard %t/arm64.ll | FileCheck %s --check-prefix=ARM64
; RUN: sed 's/windows-itanium/windows-itanium-elf/' %t/nocf.ll \
; RUN:   | opt -S -passes=cfguard | FileCheck %s --check-prefix=ELF

;; With the kcfi-marker module flag, an indirect call with a kcfi bundle goes
;; through a per-type thunk in place of the guard function, whether or not the
;; module has Control Flow Guard checks, and keeps no kcfi bundle. Other
;; indirect calls are guarded as before, so a module without checks references
;; no guard function itself.

; NOCF-NOT: __guard_

;; Only COFF images have a guard function for the thunks to continue into.
; ELF-NOT: __llvm_kcfi_
; ELF: call i32 %p(i32 %x) [ "kcfi"(i32 305419896) ]
; ELF-NOT: __llvm_kcfi_

; CHECK-LABEL: define i32 @dispatch(
; CHECK:         call i32 @__llvm_kcfi_dispatch_12345678(i32 %x) [ "cfguardtarget"(ptr %p) ]{{$}}

; CHECK-LABEL: define void @invoke(
; CHECK:         invoke void @__llvm_kcfi_dispatch_00000007() [ "cfguardtarget"(ptr %p) ]{{$}}

;; A call the dispatch function cannot guard takes the check thunk.
; CHECK-LABEL: define void @check(
; CHECK:         call cfguard_checkcc void @__llvm_kcfi_check_00000010(ptr %p)
; CHECK-NEXT:    call x86_64_sysvcc void (i32, ...) %p(i32 1){{$}}

; CHECK-LABEL: define void @unchecked(
; NOCF:          call void %p(){{$}}
; NOCF-NOT:      __guard_
; CF:            [[FN:%.*]] = load ptr, ptr @__guard_dispatch_icall_fptr
; CF-NEXT:       call void [[FN]]() [ "cfguardtarget"(ptr %p) ]

;; A call marked kcfi_local, whose every target is in this image, takes the
;; local thunk of its mechanism.
; CHECK-LABEL: define i32 @local(
; CHECK:         call i32 @__llvm_kcfi_local_dispatch_12345678(i32 %x) [ "cfguardtarget"(ptr %p) ]

; CHECK: declare hidden void @__llvm_kcfi_dispatch_12345678()
; CHECK: declare hidden void @__llvm_kcfi_dispatch_00000007()
; CHECK: declare hidden void @__llvm_kcfi_check_00000010()
; CHECK: declare hidden void @__llvm_kcfi_local_dispatch_12345678()

;; AArch64 takes the check thunk.
; ARM64-LABEL: define i32 @dispatch(
; ARM64:         call cfguard_checkcc void @__llvm_kcfi_check_12345678(ptr %p)
; ARM64-NEXT:    call i32 %p(i32 %x){{$}}
; ARM64-LABEL: define i32 @local(
; ARM64:         call cfguard_checkcc void @__llvm_kcfi_local_check_12345678(ptr %p)
; ARM64-NEXT:    call i32 %p(i32 %x)
; ARM64: declare hidden void @__llvm_kcfi_check_12345678()
; ARM64: declare hidden void @__llvm_kcfi_local_check_12345678()

;--- nocf.ll
target triple = "x86_64-unknown-windows-itanium"

declare i32 @__gxx_personality_seh0(...)

define i32 @dispatch(ptr %p, i32 %x) {
  %r = call i32 %p(i32 %x) [ "kcfi"(i32 305419896) ]
  ret i32 %r
}

define void @invoke(ptr %p) personality ptr @__gxx_personality_seh0 {
  invoke void %p() [ "kcfi"(i32 7) ] to label %ok unwind label %lp
ok:
  ret void
lp:
  %l = landingpad { ptr, i32 } cleanup
  resume { ptr, i32 } %l
}

define void @check(ptr %p) {
  call x86_64_sysvcc void (i32, ...) %p(i32 1) [ "kcfi"(i32 16) ]
  ret void
}

define void @unchecked(ptr %p) {
  call void %p()
  ret void
}

define i32 @local(ptr %p, i32 %x) {
  %r = call i32 %p(i32 %x) [ "kcfi"(i32 305419896) ], !kcfi_local !{}
  ret i32 %r
}

!llvm.module.flags = !{!0, !1}
!0 = !{i32 4, !"kcfi", i32 1}
!1 = !{i32 4, !"kcfi-marker", i32 -559038737}

;--- cf.ll
target triple = "x86_64-unknown-windows-itanium"

declare i32 @__gxx_personality_seh0(...)

define i32 @dispatch(ptr %p, i32 %x) {
  %r = call i32 %p(i32 %x) [ "kcfi"(i32 305419896) ]
  ret i32 %r
}

define void @invoke(ptr %p) personality ptr @__gxx_personality_seh0 {
  invoke void %p() [ "kcfi"(i32 7) ] to label %ok unwind label %lp
ok:
  ret void
lp:
  %l = landingpad { ptr, i32 } cleanup
  resume { ptr, i32 } %l
}

define void @check(ptr %p) {
  call x86_64_sysvcc void (i32, ...) %p(i32 1) [ "kcfi"(i32 16) ]
  ret void
}

define void @unchecked(ptr %p) {
  call void %p()
  ret void
}

define i32 @local(ptr %p, i32 %x) {
  %r = call i32 %p(i32 %x) [ "kcfi"(i32 305419896) ], !kcfi_local !{}
  ret i32 %r
}

!llvm.module.flags = !{!0, !1, !2}
!0 = !{i32 4, !"kcfi", i32 1}
!1 = !{i32 4, !"kcfi-marker", i32 -559038737}
!2 = !{i32 2, !"cfguard", i32 2}

;--- arm64.ll
target triple = "aarch64-unknown-windows-itanium"

define i32 @dispatch(ptr %p, i32 %x) {
  %r = call i32 %p(i32 %x) [ "kcfi"(i32 305419896) ]
  ret i32 %r
}

define i32 @local(ptr %p, i32 %x) {
  %r = call i32 %p(i32 %x) [ "kcfi"(i32 305419896) ], !kcfi_local !{}
  ret i32 %r
}

!llvm.module.flags = !{!0, !1}
!0 = !{i32 4, !"kcfi", i32 1}
!1 = !{i32 4, !"kcfi-marker", i32 -559038737}

; RUN: opt -S -passes=cfguard %s | FileCheck %s
; RUN: sed 's/x86_64-unknown-windows-itanium/aarch64-unknown-windows-itanium/' %s \
; RUN:   | opt -S -passes=cfguard | FileCheck %s
; RUN: sed 's/x86_64-unknown-windows-itanium/arm64ec-pc-windows-msvc/' %s \
; RUN:   | opt -S -passes=cfguard | FileCheck %s --check-prefix=KEEP

;; A virtual call whose vtable pointer load carries vtable_guard metadata goes
;; through the class's guard thunk, with the vtable pointer, not the loaded
;; function, as the dispatch target, and loses its kcfi bundle. The thunk
;; declaration carries the class, the slot offset, the type and whether every
;; target is in the image, which the backend reads. On a target other than
;; x86-64 the dispatch register does not carry the pointer, so the call keeps
;; its kcfi bundle and the vtable guard is not applied.

; CHECK-LABEL: define void @call(
; CHECK:         %vtable = load ptr, ptr %a
; CHECK:         call void @__llvm_vcall_1A_8(ptr %a) [ "cfguardtarget"(ptr %vtable) ]
; CHECK-NOT:     "kcfi"
; CHECK:       declare !vcall_thunk ![[MD:[0-9]+]] hidden void @__llvm_vcall_1A_8()
; CHECK-DAG:   ![[MD]] = !{!"1A", i64 8, i32 123456, i1 false}

; On Arm64EC the call takes the ordinary KCFI routing, not a vtable guard.
; KEEP-LABEL: define void @call(
; KEEP-NOT:     __llvm_vcall

target triple = "x86_64-unknown-windows-itanium"

define void @call(ptr %a) !kcfi_type !3 {
entry:
  %vtable = load ptr, ptr %a
  %slot = getelementptr inbounds i8, ptr %vtable, i64 8
  %f = load ptr, ptr %slot, !vtable_guard !4
  call void %f(ptr %a) [ "kcfi"(i32 123456) ]
  ret void
}

!llvm.module.flags = !{!0, !1}
!0 = !{i32 4, !"kcfi", i32 1}
!1 = !{i32 4, !"function-type-prefix", i32 -559038737}
!3 = !{i32 123456}
!4 = !{!"1A", i64 8}

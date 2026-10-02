; RUN: opt -S -passes=cfguard %s | FileCheck %s
; RUN: sed 's/x86_64-unknown-windows-itanium/aarch64-unknown-windows-itanium/' %s \
; RUN:   | opt -S -passes=cfguard | FileCheck %s
; RUN: sed 's/x86_64-unknown-windows-itanium/arm64ec-pc-windows-msvc/' %s \
; RUN:   | opt -S -passes=cfguard | FileCheck %s --check-prefix=KEEP
; RUN: sed 's/windows-itanium/windows-itanium-elf/' %s \
; RUN:   | opt -S -passes=cfguard | FileCheck %s --check-prefix=KEEP

;; With the kcfi-marker module flag, a call to llvm.kcfi.check at the type word
;; of a prefix goes through the type's check thunk, and one at the second type
;; word, which a function that can occupy a vtable slot carries, through the
;; type's vfn check thunk. Both take the target as the guard check function
;; does, whether or not the module has Control Flow Guard checks. A check at
;; another offset is left for expansion before instruction selection.

; CHECK-LABEL: define void @nonvirtual(
; CHECK-NEXT:    call cfguard_checkcc void @__llvm_kcfi_check_12345678(ptr %p)
; CHECK-NEXT:    call void %p()
; CHECK-LABEL: define void @virtual(
; CHECK-NEXT:    call cfguard_checkcc void @__llvm_kcfi_vfn_check_89abcdef(ptr %p)
; CHECK-NEXT:    call void %p()
; CHECK-LABEL: define void @other(
; CHECK-NEXT:    call void @llvm.kcfi.check(ptr %p, i32 1, i32 8)
; CHECK: declare hidden void @__llvm_kcfi_check_12345678()
; CHECK: declare hidden void @__llvm_kcfi_vfn_check_89abcdef()

;; Arm64EC and ELF have no thunks.
; KEEP-NOT: __llvm_kcfi_
; KEEP:     call void @llvm.kcfi.check(ptr %p, i32 305419896, i32 4)
; KEEP:     call void @llvm.kcfi.check(ptr %p, i32 -1985229329, i32 16)
; KEEP-NOT: __llvm_kcfi_

target triple = "x86_64-unknown-windows-itanium"

define void @nonvirtual(ptr %p) {
  call void @llvm.kcfi.check(ptr %p, i32 305419896, i32 4)
  call void %p() #0
  ret void
}

define void @virtual(ptr %p) {
  call void @llvm.kcfi.check(ptr %p, i32 -1985229329, i32 16)
  call void %p() #0
  ret void
}

define void @other(ptr %p) {
  call void @llvm.kcfi.check(ptr %p, i32 1, i32 8)
  call void %p() #0
  ret void
}

attributes #0 = { "guard_nocf" }

!llvm.module.flags = !{!0, !1}
!0 = !{i32 4, !"kcfi", i32 1}
!1 = !{i32 4, !"kcfi-marker", i32 -559038737}

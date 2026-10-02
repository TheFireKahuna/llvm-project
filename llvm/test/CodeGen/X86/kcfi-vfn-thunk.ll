; RUN: split-file %s %t
; RUN: llc -mtriple=x86_64-unknown-windows-itanium < %t/a.ll | FileCheck %s
; RUN: llc -mtriple=x86_64-unknown-windows-itanium -filetype=obj < %t/a.ll \
; RUN:   | llvm-objdump -d - | FileCheck %s --check-prefix=BYTES
; RUN: llc -mtriple=x86_64-unknown-windows-itanium < %t/prefix.ll \
; RUN:   | FileCheck %s --check-prefix=PREFIX

;; With the kcfi-marker module flag, llvm.kcfi.check at offset 16 calls the
;; vfn check thunk of its type with the target in RCX. The thunk compares the
;; 8 bytes at 16 bytes before the target, the second type that a function
;; which can occupy a vtable slot carries and the first four bytes of the
;; marker, and otherwise has the check thunk's form, with the check thunk's
;; mismatch routine. Its page test allows for a prefix of 16 bytes, and a
;; patchable-function prefix moves the compare and widens the test.

; CHECK-LABEL: virtual:
; CHECK:         callq __llvm_kcfi_vfn_check_89abcdef
; CHECK-NEXT:    callq *%rcx

; CHECK:       .weak __llvm_kcfi_check_mismatch_89abcdef
; CHECK-NEXT:  __llvm_kcfi_check_mismatch_89abcdef = __llvm_kcfi_check_default
; CHECK-NEXT:  .section .text,"xr",discard,__llvm_kcfi_vfn_check_89abcdef
; CHECK:       __llvm_kcfi_vfn_check_89abcdef:
; CHECK-NEXT:    leaq __llvm_code_start(%rip), %r10
; CHECK-NEXT:    cmpq %r10, %rcx
; CHECK-NEXT:    jb [[OUT:.Ltmp[0-9]+]]
; CHECK-NEXT:    leaq __llvm_code_end(%rip), %r10
; CHECK-NEXT:    cmpq %r10, %rcx
; CHECK-NEXT:    jae [[OUT]]
; CHECK-NEXT:    movabsq $-1188916150031102481, %r11 # imm = 0xEF801F0F89ABCDEF
; CHECK-NEXT:    cmpq %r11, -16(%rcx)
; CHECK-NEXT:    jne __llvm_kcfi_check_mismatch_89abcdef
; CHECK-NEXT:    retq
; CHECK-NEXT:  [[OUT]]:
; CHECK-NEXT:    testl $4080, %ecx
; CHECK-NEXT:    je __llvm_kcfi_check_mismatch_89abcdef
; CHECK-NEXT:    movabsq $-1188916150031102481, %r11 # imm = 0xEF801F0F89ABCDEF
; CHECK-NEXT:    cmpq %r11, -16(%rcx)
; CHECK-NEXT:    jne __llvm_kcfi_check_mismatch_89abcdef
; CHECK-NEXT:    jmpq *__guard_check_icall_fptr(%rip)

;; A second type that would spell ENDBR64 is compared as the prefix stores it,
;; plus one.
; CHECK-LABEL: __llvm_kcfi_vfn_check_fa1e0ff3:
; CHECK:         movabsq $-1188916148144566284, %r11 # imm = 0xEF801F0FFA1E0FF4

; BYTES-LABEL: <__llvm_kcfi_vfn_check_89abcdef>:
; BYTES:       18: 49 bb ef cd ab 89 0f 1f 80 ef movabsq
; BYTES-NEXT:  22: 4c 39 59 f0                   cmpq %r11, -0x10(%rcx)
; BYTES:       2d: f7 c1 f0 0f 00 00             testl $0xff0, %ecx

; PREFIX-LABEL: __llvm_kcfi_vfn_check_89abcdef:
; PREFIX:         cmpq %r11, -20(%rcx)
; PREFIX:         testl $4064, %ecx
; PREFIX-NEXT:    je __llvm_kcfi_check_mismatch_89abcdef
; PREFIX-NEXT:    movabsq $-1188916150031102481, %r11
; PREFIX-NEXT:    cmpq %r11, -20(%rcx)

;--- a.ll
define void @virtual(ptr %p) {
  call void @llvm.kcfi.check(ptr %p, i32 -1985229329, i32 16)
  call void %p() #0
  ret void
}

define void @endbr(ptr %p) {
  call void @llvm.kcfi.check(ptr %p, i32 -98693133, i32 16)
  call void %p() #0
  ret void
}

attributes #0 = { "guard_nocf" }

!llvm.module.flags = !{!0, !1}
!0 = !{i32 4, !"kcfi", i32 1}
!1 = !{i32 4, !"kcfi-marker", i32 -559038737}

;--- prefix.ll
define void @virtual(ptr %p) {
  call void @llvm.kcfi.check(ptr %p, i32 -1985229329, i32 16)
  call void %p() #0
  ret void
}

attributes #0 = { "guard_nocf" }

!llvm.module.flags = !{!0, !1, !2}
!0 = !{i32 4, !"kcfi", i32 1}
!1 = !{i32 4, !"kcfi-marker", i32 -559038737}
!2 = !{i32 4, !"kcfi-offset", i32 4}

; RUN: llc -mtriple=x86_64-unknown-windows-itanium < %s | FileCheck %s
; RUN: llc -mtriple=x86_64-unknown-windows-itanium -filetype=obj < %s \
; RUN:   | llvm-readobj --symbols - | FileCheck %s --check-prefix=SYMS

;; With the kcfi-marker module flag, a call with a KCFI type goes through a
;; per-type thunk, a COMDAT that checks the type before the target and
;; continues into the guard function, or on a mismatch into a weak alias of the
;; routine that fails fast if the target carries the marker. The dispatch
;; thunk takes the target in RAX, and the check thunk in RCX.

; CHECK-LABEL: f1:
; CHECK:         movq %rcx, %rax
; CHECK:         callq __llvm_kcfi_dispatch_12345678
define void @f1(ptr %p) {
  call void %p() [ "kcfi"(i32 305419896) ]
  ret void
}

; CHECK-LABEL: f2:
; CHECK:         movq %rcx, %rax
; CHECK:         jmp __llvm_kcfi_dispatch_12345678 # TAILCALL
define void @f2(ptr %p) {
  tail call void %p() [ "kcfi"(i32 305419896) ]
  ret void
}

; CHECK-LABEL: f3:
; CHECK:         movq %rdi, %rcx
; CHECK-NEXT:    callq __llvm_kcfi_check_00000010
; CHECK-NEXT:    xorl %eax, %eax
; CHECK-NEXT:    callq *%rdi
define x86_64_sysvcc void @f3(ptr %p) nounwind {
  call x86_64_sysvcc void (...) %p() [ "kcfi"(i32 16) ]
  ret void
}

; CHECK:       .weak __llvm_kcfi_mismatch_12345678
; CHECK-NEXT:  __llvm_kcfi_mismatch_12345678 = __llvm_kcfi_open
; CHECK-NEXT:  .section .text,"xr",discard,__llvm_kcfi_dispatch_12345678
; CHECK:       .globl __llvm_kcfi_dispatch_12345678
; CHECK-NEXT:  .p2align 4
; CHECK-NEXT:  __llvm_kcfi_dispatch_12345678:
; CHECK-NEXT:    cmpl $305419896, -4(%rax)
; CHECK-NEXT:    jne __llvm_kcfi_mismatch_12345678
; CHECK-NEXT:    jmpq *__guard_dispatch_icall_fptr(%rip)
; CHECK-NEXT:  .section .text,"xr",discard,__llvm_kcfi_open
; CHECK:       .globl __llvm_kcfi_open
; CHECK-NEXT:  .p2align 4
; CHECK-NEXT:  __llvm_kcfi_open:
; CHECK-NEXT:    movabsq $-5125468290327503089, %r11 # imm = 0xB8DEADBEEF801F0F
; CHECK-NEXT:    cmpq %r11, -12(%rax)
; CHECK-NEXT:    je [[TRAP:.Ltmp[0-9]+]]
; CHECK-NEXT:    jmpq *__guard_dispatch_icall_fptr(%rip)
; CHECK-NEXT:  [[TRAP]]:
; CHECK-NEXT:    movl $64, %ecx
; CHECK-NEXT:    int $41

; CHECK:       .weak __llvm_kcfi_check_mismatch_00000010
; CHECK-NEXT:  __llvm_kcfi_check_mismatch_00000010 = __llvm_kcfi_check_open
; CHECK-NEXT:  .section .text,"xr",discard,__llvm_kcfi_check_00000010
; CHECK:       __llvm_kcfi_check_00000010:
; CHECK-NEXT:    cmpl $16, -4(%rcx)
; CHECK-NEXT:    jne __llvm_kcfi_check_mismatch_00000010
; CHECK-NEXT:    jmpq *__guard_check_icall_fptr(%rip)
; CHECK:       __llvm_kcfi_check_open:
; CHECK-NEXT:    movabsq $-5125468290327503089, %r11 # imm = 0xB8DEADBEEF801F0F
; CHECK-NEXT:    cmpq %r11, -12(%rcx)
; CHECK-NEXT:    je [[TRAP:.Ltmp[0-9]+]]
; CHECK-NEXT:    jmpq *__guard_check_icall_fptr(%rip)
; CHECK-NEXT:  [[TRAP]]:
; CHECK-NEXT:    movl $64, %ecx
; CHECK-NEXT:    int $41

; SYMS:      Name: __llvm_kcfi_mismatch_12345678
; SYMS-NEXT: Value: 0
; SYMS-NEXT: Section: IMAGE_SYM_UNDEFINED (0)
; SYMS:      StorageClass: WeakExternal (0x69)
; SYMS:      Linked: __llvm_kcfi_open

!llvm.module.flags = !{!0, !1}
!0 = !{i32 4, !"kcfi", i32 1}
!1 = !{i32 4, !"kcfi-marker", i32 -559038737}

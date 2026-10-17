; REQUIRES: x86

;; The KCFI thunks that llc emits carry the records by which the linker,
;; in an image it seals, replaces them with its own form: the
;; range test as one comparison of the target's offset from the start of .text
;; with its size, then clang's type check, which it builds from the records.

; RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
; RUN: llc -filetype=obj main.ll -o main.obj
; RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc guard.s -o guard.obj
; RUN: lld-link main.obj guard.obj -guard:cf -entry:main \
; RUN:   -debug:symtab -opt:ref -out:main.exe
; RUN: llvm-objdump -d main.exe | FileCheck %s

; CHECK:      <__llvm_kcfi_dispatch_22222222>:
; CHECK-NEXT:   leaq {{.*}}(%rip), %r10 {{.*}}0x140001000
; CHECK-NEXT:   movq %rax, %r11
; CHECK-NEXT:   subq %r10, %r11
; CHECK-NEXT:   cmpq $0x{{[0-9a-f]+}}, %r11
; CHECK-NEXT:   jae
; CHECK-NEXT:   movabsq $0x22222222b8071c5a, %r11
; CHECK-NEXT:   cmpq %r11, -0x8(%rax)
; CHECK-NEXT:   jne {{.*}}<__llvm_kcfi_trap>
; CHECK-NEXT:   jmpq *%rax
; CHECK-NEXT:   testl $0xff0, %eax
; CHECK-NEXT:   je {{.*}}<__llvm_kcfi_trap>
; CHECK-NEXT:   movabsq $0x22222222b8071c5a, %r11
; CHECK-NEXT:   cmpq %r11, -0x8(%rax)
; CHECK-NEXT:   jne {{.*}}<__llvm_kcfi_trap>
; CHECK-NEXT:   jmpq *{{.*}}(%rip) {{.*}}<__guard_dispatch_icall_fptr>

;--- main.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-windows-itanium"

@fp = global ptr @callee

define void @callee() !kcfi_type !0 { ret void }

define void @main() !kcfi_type !0 {
  %p = load volatile ptr, ptr @fp
  call void %p() [ "kcfi"(i32 572662306) ]
  ret void
}

!0 = !{i32 572662306}
!llvm.module.flags = !{!1, !2, !3}
!1 = !{i32 4, !"kcfi", i32 1}
!2 = !{i32 4, !"function-type-prefix", i32 119298566}
!3 = !{i32 2, !"cfguard", i32 2}

;--- guard.s
        .globl @feat.00
@feat.00 = 0x800

        .section .rdata,"dr"
        .p2align 3
        .globl __guard_check_icall_fptr
__guard_check_icall_fptr:
        .quad 0
        .globl __guard_dispatch_icall_fptr
__guard_dispatch_icall_fptr:
        .quad 0
        .globl _load_config_used
_load_config_used:
        .long 256
        .fill 124, 1, 0
        .quad __guard_fids_table
        .quad __guard_fids_count
        .long __guard_flags
        .fill 128, 1, 0

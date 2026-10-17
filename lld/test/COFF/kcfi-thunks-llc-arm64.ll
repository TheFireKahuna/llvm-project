; REQUIRES: aarch64

;; On ARM64, the KCFI check thunks that llc emits carry the records by which the
;; linker, in an image it seals, replaces them with its own
;; form: the range test as one comparison of the target's offset from the start
;; of .text with its size, then clang's type check, which it builds from the
;; records.

; RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
; RUN: llc -filetype=obj main.ll -o main.obj
; RUN: llvm-mc -filetype=obj -triple=aarch64-windows-msvc guard.s -o guard.obj
; RUN: lld-link main.obj guard.obj -machine:arm64 -guard:cf \
; RUN:   -entry:main -debug:symtab -opt:ref -out:main.exe
; RUN: llvm-objdump -d main.exe | FileCheck %s

; CHECK:      <__llvm_kcfi_check_22222222>:
; CHECK-NEXT:   adrp x16, 0x140001000
; CHECK-NEXT:   add x16, x16, #0x{{[0-9a-f]+}}
; CHECK-NEXT:   sub x16, x15, x16
; CHECK-NEXT:   mov x17, #0x{{[0-9a-f]+}}
; CHECK-NEXT:   movk x17, #0x0, lsl #16
; CHECK-NEXT:   cmp x16, x17
; CHECK-NEXT:   b.hs
; CHECK-NEXT:   ldur x16, [x15, #-0x8]
; CHECK-NEXT:   mov x17, #0x1c5a
; CHECK-NEXT:   movk x17, #0xb807, lsl #16
; CHECK-NEXT:   movk x17, #0x2222, lsl #32
; CHECK-NEXT:   movk x17, #0x2222, lsl #48
; CHECK-NEXT:   cmp x16, x17
; CHECK-NEXT:   b.ne
; CHECK-NEXT:   ret
; CHECK-NEXT:   tst x15, #0xff0
; CHECK-NEXT:   b.eq
; CHECK-NEXT:   ldur x16, [x15, #-0x8]
; CHECK-NEXT:   mov x17, #0x1c5a
; CHECK-NEXT:   movk x17, #0xb807, lsl #16
; CHECK-NEXT:   movk x17, #0x2222, lsl #32
; CHECK-NEXT:   movk x17, #0x2222, lsl #48
; CHECK-NEXT:   cmp x16, x17
; CHECK-NEXT:   b.ne
; CHECK-NEXT:   adrp x16,
; CHECK-NEXT:   ldr x16, [x16{{(, #0x[0-9a-f]+)?}}]
; CHECK-NEXT:   br x16

;--- main.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "aarch64-unknown-windows-itanium"

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
        .section .rdata,"dr"
        .p2align 3
        .globl __guard_check_icall_fptr
__guard_check_icall_fptr:
        .xword 0
        .globl __guard_dispatch_icall_fptr
__guard_dispatch_icall_fptr:
        .xword 0
        .globl _load_config_used
_load_config_used:
        .word 320
        .fill 108, 1, 0
        .xword __guard_check_icall_fptr
        .xword __guard_dispatch_icall_fptr
        .xword __guard_fids_table
        .xword __guard_fids_count
        .word __guard_flags
        .fill 180, 1, 0

; REQUIRES: x86, aarch64
; RUN: rm -rf %t && split-file %s %t

;; Code generation lists the return sites of setjmp in .gljmp$y for Windows
;; Itanium and NT-POSIX as it does for MSVC, and -guard:cf, as the drivers pass
;; it, writes the longjmp table and sets its load configuration flag.

; RUN: llvm-mc -triple x86_64-windows-msvc -filetype=obj %t/ldcfg-x64.s \
; RUN:   -o %t/ldcfg-x64.obj
; RUN: llvm-mc -triple x86_64-windows-msvc -filetype=obj %t/setjmp-x64.s \
; RUN:   -o %t/setjmp-x64.obj
; RUN: llvm-as %t/x64.ll -o %t/x64.bc
; RUN: lld-link -entry:main -guard:cf,exportsuppress %t/x64.bc \
; RUN:   %t/setjmp-x64.obj %t/ldcfg-x64.obj -out:%t/x64.exe
; RUN: llvm-readobj --coff-load-config %t/x64.exe | FileCheck %s

; RUN: sed 's/x86_64-unknown-windows-itanium/x86_64-pc-windows-ntposix/' \
; RUN:   %t/x64.ll | llvm-as -o %t/ntposix.bc
; RUN: lld-link -entry:main -guard:cf,exportsuppress %t/ntposix.bc \
; RUN:   %t/setjmp-x64.obj %t/ldcfg-x64.obj -out:%t/ntposix.exe
; RUN: llvm-readobj --coff-load-config %t/ntposix.exe | FileCheck %s

; RUN: llvm-mc -triple aarch64-windows-msvc -filetype=obj \
; RUN:   %S/Inputs/loadconfig-arm64.s -o %t/ldcfg-arm64.obj
; RUN: llvm-mc -triple aarch64-windows-msvc -filetype=obj %t/setjmp-arm64.s \
; RUN:   -o %t/setjmp-arm64.obj
; RUN: llvm-as %t/arm64.ll -o %t/arm64.bc
; RUN: lld-link -entry:main -guard:cf,exportsuppress %t/arm64.bc \
; RUN:   %t/setjmp-arm64.obj %t/ldcfg-arm64.obj -out:%t/arm64.exe
; RUN: llvm-readobj --coff-load-config %t/arm64.exe | FileCheck %s

; CHECK:      LoadConfig [
; CHECK:        GuardFlags [
; CHECK:          CF_LONGJUMP_TABLE_PRESENT (0x10000)
; CHECK:        ]
; CHECK:        GuardLongJumpTargetTable: 0x{{[1-9A-F][0-9A-F]*}}
; CHECK-NEXT:   GuardLongJumpTargetCount: 1
; CHECK:      ]
; CHECK:      GuardLJmpTable [
; CHECK-NEXT:   0x{{[0-9A-F]+}}
; CHECK-NEXT: ]

;--- x64.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-windows-itanium"

@buf = internal global [32 x i64] zeroinitializer, align 16
@res = internal global i32 0

declare i32 @_setjmp(ptr) returns_twice

define i32 @main() {
  %r = call i32 @_setjmp(ptr @buf)
  store volatile i32 %r, ptr @res
  ret i32 0
}

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"cfguard", i32 2}

;--- ldcfg-x64.s
        .section .rdata,"dr"
        .globl _load_config_used
_load_config_used:
        .long 256
        .fill 124, 1, 0
        .quad __guard_fids_table
        .quad __guard_fids_count
        .long __guard_flags
        .fill 12, 1, 0
        .quad __guard_iat_table
        .quad __guard_iat_count
        .quad __guard_longjmp_table
        .quad __guard_longjmp_count
        .fill 84, 1, 0

;--- setjmp-x64.s
        .text
        .globl _setjmp
_setjmp:
        xorl %eax, %eax
        retq

;--- setjmp-arm64.s
        .text
        .globl _setjmp
_setjmp:
        mov w0, #0
        ret

;--- arm64.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-p:64:64-i32:32-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "aarch64-unknown-windows-itanium"

@buf = internal global [32 x i64] zeroinitializer, align 16
@res = internal global i32 0

declare i32 @_setjmp(ptr) returns_twice

define i32 @main() {
  %r = call i32 @_setjmp(ptr @buf)
  store volatile i32 %r, ptr @res
  ret i32 0
}

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"cfguard", i32 2}

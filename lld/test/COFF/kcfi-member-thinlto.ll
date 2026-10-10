; REQUIRES: x86

;; With ThinLTO, the thin link gives the membership tags: a function that one
;; module defines carries its tag in its KCFI prefix, and a call that another
;; module checks by the type's tags goes through the member thunk that tests
;; it. Without a native object of the type, a miss is the trap. A function of
;; the type whose address no module takes is a member when the image exports
;; it, and not when it is only called. A member with internal linkage, which
;; splitting the module promotes through an alias, carries its tag too.

; RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
; RUN: opt -thinlto-bc -thinlto-split-lto-unit a.ll -o a.bc
; RUN: opt -thinlto-bc -thinlto-split-lto-unit b.ll -o b.bc
; RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc guard.s -o guard.obj
; RUN: lld-link a.bc b.bc guard.obj -guard:cf -entry:main \
; RUN:   -export:exported -export:get_local -debug:symtab -opt:ref -lldsavetemps -out:t.exe
; RUN: llvm-dis a.bc.5.precodegen.bc -o - | FileCheck %s --check-prefix=IR
; RUN: llvm-objdump -s -j .text -d t.exe | FileCheck %s

;; The word before the marker of member's prefix is the tag, little-endian.
; CHECK:      Contents of section .text:
; CHECK-NEXT:   140001000 [[T0:[0-9a-f]{2}]][[T1:[0-9a-f]{2}]][[T2:[0-9a-f]{2}]][[T3:[0-9a-f]{2}]] 0f1f8006
; CHECK:      <__llvm_code_start>:
; CHECK:      <member>:
; CHECK:      <call>:
; CHECK:        jmp {{.*}} <__llvm_kcfi_member_dispatch_22222222_[[T3]][[T2]][[T1]][[T0]]>
; CHECK:      <__llvm_kcfi_member_dispatch_22222222_[[T3]][[T2]][[T1]][[T0]]>:
; CHECK:        movabsq $0x6801f0f[[T3]][[T2]][[T1]][[T0]], %r11
; CHECK-NEXT:   cmpq %r11, -0x10(%rax)
; CHECK-NEXT:   je
; CHECK-NEXT:   jmp {{.*}} <__llvm_kcfi_trap>

; IR: define {{.*}}void @member() {{.*}}!kcfi_member_tag
; IR: define internal void @local() {{.*}}!kcfi_member_tag
; IR: define {{.*}}void @exported() {{.*}}!kcfi_member_tag
; IR: define {{.*}}void @called() {{.*}}!kcfi_type !{{[0-9]+}} !guid !{{[0-9]+}} {{[{]$}}

;--- a.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-windows-itanium"

@fp = global ptr @member
@lp = private unnamed_addr constant ptr @local

define void @member() !type !0 !kcfi_type !10 { ret void }
define internal void @local() !type !0 !kcfi_type !10 { ret void }
define ptr @get_local() {
  %p = load ptr, ptr @lp
  ret ptr %p
}
define void @exported() !type !0 !kcfi_type !10 { ret void }
define void @called() noinline !type !0 !kcfi_type !10 { ret void }

!0 = !{i64 0, !"typeid"}
!10 = !{i32 572662306}
!llvm.module.flags = !{!1, !2, !3}
!1 = !{i32 4, !"kcfi", i32 1}
!2 = !{i32 4, !"function-type-prefix", i32 119298566}
!3 = !{i32 2, !"cfguard", i32 2}

;--- b.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-windows-itanium"

@fp = external global ptr

define void @call(ptr %p) noinline !kcfi_type !10 {
  %t = call i1 @llvm.type.test(ptr %p, metadata !"typeid")
  br i1 %t, label %cont, label %trap
trap:
  call void @llvm.ubsantrap(i8 64)
  unreachable
cont:
  call void %p() [ "kcfi"(i32 572662306) ]
  ret void
}

define void @main() !kcfi_type !10 {
  %p = load volatile ptr, ptr @fp
  call void @call(ptr %p)
  call void @called()
  ret void
}

declare void @called()

declare i1 @llvm.type.test(ptr, metadata)
declare void @llvm.ubsantrap(i8)

!10 = !{i32 572662306}
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

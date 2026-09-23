; RUN: llvm-as %s -o %t.bc
; RUN: llvm-lto2 dump-symtab %t.bc | FileCheck %s
; RUN: opt -module-summary %s -o %t.thin.bc
; RUN: llvm-lto2 dump-symtab %t.thin.bc | FileCheck %s
; RUN: sed 's/i32 3/i32 7/' %s > %t.bad.ll
; RUN: llvm-as %t.bad.ll -o %t.bad.bc
; RUN: not llvm-lto2 dump-symtab %t.bad.bc 2>&1 | FileCheck %s --check-prefix=BAD

target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-itanium"

; Binding policy uses existing flag space, with no extra per-symbol record.
; Weak aliases do not unconditionally inherit their fallback's requirement.
@descriptor = constant [2 x ptr] zeroinitializer, !coff.binding !1
@name = constant [2 x i8] c"T\00", !coff.binding !2
@fallback = weak alias [2 x ptr], ptr @descriptor

; CHECK: descriptor
; CHECK-NEXT: {{ *}}coff binding 3
; CHECK: name
; CHECK-NEXT: {{ *}}coff binding 5
; CHECK: fallback
; CHECK-NEXT: {{ *}}fallback descriptor
; CHECK-NOT: coff binding
; BAD: invalid COFF binding metadata for descriptor

!llvm.module.flags = !{!0}
!0 = !{i32 1, !"coff.rtti_abi", i32 2}
!1 = !{i32 3}
!2 = !{i32 5}

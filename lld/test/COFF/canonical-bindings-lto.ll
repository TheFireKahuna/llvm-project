; REQUIRES: x86
; RUN: split-file %s %t
; RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %t/provider.s -o %t/provider.obj
; RUN: lld-link -dll -noentry %t/provider.obj -out:%t/provider.dll -implib:%t/provider.lib -export:entity,DATA
; RUN: llvm-as %t/consumer.ll -o %t/consumer.bc
; RUN: lld-link -dll -noentry -auto-import -import-slots -lldsavetemps %t/consumer.bc %t/provider.lib -out:%t/full.dll
; RUN: llvm-readobj --coff-imports %t/full.dll | FileCheck %s --check-prefix=IMPORT
; RUN: FileCheck %s --check-prefix=RESOLUTION < %t/full.dll.resolution.txt
; RUN: opt -module-summary %t/consumer.ll -o %t/consumer.thin.bc
; RUN: lld-link -dll -noentry -auto-import -import-slots %t/provider.lib %t/consumer.thin.bc -out:%t/thin.dll
; RUN: llvm-readobj --coff-imports %t/thin.dll | FileCheck %s --check-prefix=IMPORT
; RUN: llvm-ar crs %t/consumer.lib %t/consumer.bc
; RUN: lld-link -dll -noentry -auto-import -import-slots -include:slot %t/consumer.lib %t/provider.lib -out:%t/archive.dll
; RUN: llvm-readobj --coff-imports %t/archive.dll | FileCheck %s --check-prefix=IMPORT
; RUN: llvm-as %t/conflict.ll -o %t/conflict.bc
; RUN: env LLD_IN_TEST=1 not lld-link -dll -noentry -auto-import -import-slots %t/consumer.bc %t/conflict.bc %t/provider.lib -out:%t/bad.dll 2>&1 | FileCheck %s --check-prefix=CONFLICT
; RUN: env LLD_IN_TEST=1 not lld-link -dll -noentry -auto-import -import-slots %t/conflict.bc %t/consumer.thin.bc %t/provider.lib -out:%t/reverse.dll 2>&1 | FileCheck %s --check-prefix=CONFLICT
; RUN: llc -filetype=obj %t/conflict.ll -o %t/conflict.obj
; RUN: env LLD_IN_TEST=1 not lld-link -dll -noentry -auto-import -import-slots %t/consumer.bc %t/conflict.obj %t/provider.lib -out:%t/mixed.dll 2>&1 | FileCheck %s --check-prefix=CONFLICT
; RUN: llvm-as %t/dead.ll -o %t/dead.bc
; RUN: lld-link -dll -noentry -auto-import -import-slots -opt:ref %t/dead.bc %t/provider.lib -out:%t/dead.dll
; RUN: llvm-readobj --coff-imports %t/dead.dll | FileCheck %s --check-prefix=DEAD

; A semantic requirement must discover its provider before LTO even when there
; is no .refptr. A losing bitcode definition's requirement must be checked before
; LTO discards it. The alias must not regain local finality when its fallback's
; prevailing symbol is temporarily undefined by the LTO resolution API.
; IMPORT: Name: provider.dll
; IMPORT: Symbol: entity (0)
; RESOLUTION: -r={{.*}},entity,px
; RESOLUTION: -r={{.*}},alias,x
; CONFLICT: conflicting binding kinds for entity (resolved to entity)
; CONFLICT-SAME: previous requirement in
; DEAD: Format: COFF-x86-64
; DEAD-NOT: Import {

;--- provider.s
.section .rdata,"dr"
.globl entity
entity:
.quad 0, 0

;--- consumer.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-itanium"
$entity = comdat any
@entity = linkonce_odr constant [2 x ptr] zeroinitializer, comdat, !coff.binding !1
@alias = weak alias [2 x ptr], ptr @entity
@slot = dllexport constant ptr @alias
!llvm.module.flags = !{!0}
!0 = !{i32 1, !"coff.rtti_abi", i32 2}
!1 = !{i32 3}

;--- conflict.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-itanium"
$entity = comdat any
@entity = linkonce_odr constant [2 x ptr] zeroinitializer, comdat, !coff.binding !1
!llvm.module.flags = !{!0}
!0 = !{i32 1, !"coff.rtti_abi", i32 2}
!1 = !{i32 5}

;--- dead.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-itanium"
$entity = comdat any
@entity = linkonce_odr constant [2 x ptr] zeroinitializer, comdat, !coff.binding !1
define dllexport i32 @live() {
  ret i32 0
}
!llvm.module.flags = !{!0}
!0 = !{i32 1, !"coff.rtti_abi", i32 2}
!1 = !{i32 3}

; REQUIRES: x86
; RUN: opt -passes='default<O2>' -S %s -o %t.ll
; RUN: FileCheck %s --check-prefix=IR < %t.ll
; RUN: opt -passes=coff-output-locality -S %t.ll -o %t.final.ll
; RUN: FileCheck %s --check-prefix=CLEAN < %t.final.ll
; RUN: llc -filetype=obj %t.ll -o %t.obj
; RUN: lld-link /dll /noentry /import-slots /out:%t.dll %t.obj
; RUN: llvm-readobj --coff-imports %t.dll | FileCheck %s --check-prefix=IMPORT
; RUN: llvm-as %t.ll -o %t.bc
; RUN: lld-link /dll /noentry /import-slots /out:%t.full.dll %t.bc
; RUN: llvm-readobj --coff-imports %t.full.dll | FileCheck %s --check-prefix=IMPORT
; RUN: opt -module-summary %t.ll -o %t.thin.bc
; RUN: lld-link /dll /noentry /import-slots /out:%t.thin.dll %t.thin.bc
; RUN: llvm-readobj --coff-imports %t.thin.dll | FileCheck %s --check-prefix=IMPORT
; RUN: lld-link /dll /noentry /import-slots /lldrttiprivate:entity /out:%t.private.dll %t.obj
; RUN: llvm-readobj --coff-imports %t.private.dll | FileCheck %s --check-prefix=PRIVATE

; The load and helper both disappear; the exported constant result still
; depends on the provider contract. A dead consumer adds no native import.
; A private, jointly checked identity adds no witness slot or DLL dependency.
; IR-NOT: define internal {{.*}} @helper
; IR: define {{.*}} @answer() {{.*}} !coff.abi.uses
; IR-NEXT: ret i64 8
; IMPORT: Name: rtti2-
; IMPORT: Symbol: entity$abi$1$
; IMPORT-NOT: Symbol: dead_entity
; PRIVATE-NOT: Import {
; CLEAN-NOT: @dead_
; CLEAN: @entity =
; CLEAN-NOT: @dead_
; CLEAN: @explicitly_retained =
; CLEAN-NOT: @dead_
; CLEAN-NOT: @llvm.coff.abi.keep

target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-windows-itanium"
$entity = comdat any
$dead_entity = comdat any
@entity = linkonce_odr constant i64 8, comdat, !coff.binding !1, !coff.abi !2
@dead_entity = linkonce_odr constant {i64, ptr} {i64 8, ptr @dead_name}, comdat, !coff.binding !1, !coff.abi !3
@dead_name = linkonce_odr constant [2 x i8] c"U\00"
@dead_external = external constant i64, !coff.binding !1, !coff.abi !3
@explicitly_retained = internal constant i64 8, !coff.abi !3
@llvm.compiler.used = appending global [1 x ptr] [ptr @explicitly_retained], section "llvm.metadata"
define internal i64 @helper() {
  %size = load i64, ptr @entity
  ret i64 %size
}
define dllexport i64 @answer() {
  %size = call i64 @helper()
  ret i64 %size
}
define internal i64 @unused() {
  %size = load i64, ptr @dead_entity
  %other = load i64, ptr @dead_external
  %retained = load i64, ptr @explicitly_retained
  %a = add i64 %size, %other
  %b = add i64 %a, %retained
  ret i64 %b
}
!llvm.module.flags = !{!0}
!0 = !{i32 1, !"coff.rtti_abi", i32 2}
!1 = !{i32 3}
!2 = !{!"\01\01\02\00\01T\02\08\08\00"}
!3 = !{!"\01\01\02\00\01U\02\08\08\00"}

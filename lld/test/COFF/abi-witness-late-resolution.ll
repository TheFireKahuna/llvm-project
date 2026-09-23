; REQUIRES: x86
; RUN: opt -passes='default<O2>' %s -o %t.bc
; RUN: lld-link /dll /noentry /import-slots /out:%t.dll %t.bc
; RUN: llvm-readobj --coff-imports %t.dll | FileCheck %s --check-prefix=DEAD
; RUN: opt -module-summary %t.bc -o %t.thin.bc
; RUN: lld-link /dll /noentry /import-slots /out:%t.thin.dll %t.thin.bc
; RUN: llvm-readobj --coff-imports %t.thin.dll | FileCheck %s --check-prefix=DEAD
; RUN: env LLD_IN_TEST=1 not lld-link /dll /noentry /import-slots /export:unused /out:%t.live.dll %t.bc 2>&1 | FileCheck %s --check-prefix=LIVE
; RUN: env LLD_IN_TEST=1 not lld-link /dll /noentry /import-slots /export:unused /force:unresolved /out:%t.force.dll %t.bc 2>&1 | FileCheck %s --check-prefix=FORCE
; RUN: sed 's/external constant i64/constant i64 8/' %s > %t.unoffered.ll
; RUN: llvm-as %t.unoffered.ll -o %t.unoffered.bc
; RUN: env LLD_IN_TEST=1 not lld-link /dll /noentry /import-slots /export:unused /out:%t.unoffered.dll %t.unoffered.bc 2>&1 | FileCheck %s --check-prefix=UNOFFERED
; RUN: llc -filetype=obj %t.unoffered.ll -o %t.unoffered.obj
; RUN: env LLD_IN_TEST=1 not lld-link /dll /noentry /import-slots /export:unused /out:%t.unoffered.dll %t.unoffered.obj 2>&1 | FileCheck %s --check-prefix=UNOFFERED

; The function is externally visible at pre-link, but has no consumer or
; export at final link. Its temporary witness must not prevent LTO from
; deleting it. Exporting that same function makes the requirement mandatory.
; DEAD-NOT: Import {
; LIVE: undefined symbol: nonexistent
; FORCE: canonical binding nonexistent requires a real definition
; A consumer's requirement cannot stand in for the definition's offered facts.
; UNOFFERED: selects a definition without an ABI contract
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-windows-itanium"
@nonexistent = external constant i64, !coff.binding !1
define dllexport i32 @entry() {
  ret i32 0
}
define i64 @unused() !coff.abi.uses !3 {
  ret i64 8
}
!llvm.module.flags = !{!0}
!0 = !{i32 1, !"coff.rtti_abi", i32 2}
!1 = !{i32 3}
!2 = !{!"\01\01\02\00\01T\02\08\08\00"}
!3 = !{!4}
!4 = !{ptr @nonexistent, !"\01\01\02\00\01T\02\08\08\00"}

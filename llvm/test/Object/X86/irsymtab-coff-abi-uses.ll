; RUN: llvm-as %s -o %t.bc
; RUN: llvm-lto2 dump-symtab %t.bc | FileCheck %s
; RUN: opt -module-summary %s -o %t.thin.bc
; RUN: llvm-lto2 dump-symtab %t.thin.bc | FileCheck %s
; RUN: env LLVM_OVERRIDE_PRODUCER=rebuild llvm-lto2 dump-symtab %t.bc | FileCheck %s

; Only distinct use-only requirements need table rows. A target's own contract
; already represents equal consumer requirements. Local symbols are filtered
; by the LTO interface without shifting later target indices or losing aliases.
; Rebuilding the symtab from lazily read IR must preserve the same requirements.
; CHECK: version: 7
; CHECK: coff abi requirement target [[HASH:[A-Za-z0-9_-]{43}]]
; CHECK-NEXT: coff abi requirement fallback [[HASH]]
; CHECK-NOT: coff abi requirement

target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-itanium"
@local = internal constant i32 0
@target = external constant i64
@offered = constant i64 8, !coff.abi !0
@fallback = weak alias i64, ptr @offered

define void @consumer() !coff.abi.uses !1 {
  ret void
}
define void @other_consumer() !coff.abi.uses !1 {
  ret void
}
!0 = !{!"\01\01\02\00\01T\02\08\08\00"}
!1 = !{!2, !3, !4, !5}
!2 = !{ptr @target, !"\01\01\02\00\01T\02\08\08\00"}
!3 = !{ptr @offered, !"\01\01\02\00\01T\02\08\08\00"}
!4 = !{ptr @fallback, !"\01\01\02\00\01T\02\08\08\00"}
!5 = !{ptr @local, !"\01\01\02\00\01T\02\08\08\00"}

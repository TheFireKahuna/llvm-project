; REQUIRES: x86
; RUN: opt -passes='default<O2>' %s -o %t.bc
; RUN: llc -filetype=obj %t.bc -o %t.obj
; RUN: lld-link /dll /noentry /import-slots /opt:icf /out:%t.dll %t.obj
; RUN: llvm-readobj --coff-imports --coff-exports %t.dll | FileCheck %s

; Equal machine code with different assumptions may fold. Neither qualified
; check may disappear when its consumer's section is replaced by the other.
; CHECK-DAG: Symbol: a$abi$1$
; CHECK-DAG: Symbol: b$abi$1$
; CHECK: Name: first
; CHECK-NEXT: RVA: [[RVA:0x[0-9A-F]+]]
; CHECK: Name: second
; CHECK-NEXT: RVA: [[RVA]]
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-windows-itanium"
$a = comdat any
$b = comdat any
$first = comdat any
$second = comdat any
@a = linkonce_odr constant i64 8, comdat, !coff.binding !1, !coff.abi !2
@b = linkonce_odr constant i64 8, comdat, !coff.binding !1, !coff.abi !3
define dllexport i64 @first() comdat {
  %v = load i64, ptr @a
  ret i64 %v
}
define dllexport i64 @second() comdat {
  %v = load i64, ptr @b
  ret i64 %v
}
!llvm.module.flags = !{!0}
!0 = !{i32 1, !"coff.rtti_abi", i32 2}
!1 = !{i32 3}
!2 = !{!"\01\01\02\00\01T\02\08\08\00"}
!3 = !{!"\01\01\02\00\01U\02\08\08\00"}

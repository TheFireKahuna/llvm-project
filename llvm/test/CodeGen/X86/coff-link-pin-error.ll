; RUN: not llc -mtriple=x86_64-unknown-windows-itanium -filetype=null < %s \
; RUN:   2>&1 | FileCheck %s

; A required pin that the global's alignment contradicts is an error.
; CHECK: error: pin of '_ZTV1D' conflicts with its alignment
@_ZTV1D = constant [5 x ptr] zeroinitializer, align 64, !pin !0

!0 = !{i64 16, i64 12, i64 4072, i64 1}

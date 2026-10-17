; RUN: llc -mtriple=aarch64-unknown-windows-itanium < %s | FileCheck %s
; RUN: llc -mtriple=aarch64-unknown-windows-itanium -filetype=obj < %s | \
; RUN:   llvm-objdump -s -j .llvm_link_records - | FileCheck --check-prefix=OBJ %s

; CHECK-LABEL: _ZTV1D:
; CHECK:       .linkpin _ZTV1D, 12, 4072, required
@_ZTV1D = constant [5 x ptr] zeroinitializer, align 8, !pin !0

; "LLRC", version 1, the call-only capability (2), then a group of kind 1.
; OBJ:      Contents of section .llvm_link_records:
; OBJ-NEXT: 0000 4c4c5243 010201

!0 = !{i64 16, i64 12, i64 4088, i64 1}

; RUN: llc -mtriple=aarch64-unknown-windows-itanium < %s | FileCheck %s
; RUN: llc -mtriple=aarch64-unknown-windows-itanium \
; RUN:   -filetype=obj < %s | llvm-objdump -s -j .llvm_link_records - | \
; RUN:   FileCheck --check-prefix=OBJ %s
; RUN: llc -mtriple=aarch64-unknown-linux-gnu < %s | \
; RUN:   FileCheck --check-prefix=ELF %s

; Each vtable a global holds is described by its symbol, the offset of its
; primary address point, its class and its class's primary base.
; CHECK-LABEL: _ZTV1B:
; CHECK:       .linkvtable _ZTV1B, 16, "1B", ""
@_ZTV1B = constant [3 x ptr] zeroinitializer, align 64, !vtable_hierarchy !0

; CHECK-LABEL: _ZTV1D:
; CHECK:       .linkvtable _ZTV1D, 32, "1D", "1B"
@_ZTV1D = constant [5 x ptr] zeroinitializer, align 8, !vtable_hierarchy !1

; ELF-NOT: .linkvtable

; "LLRC", version 1, the AArch64 capabilities, then a group of kind 9.
; OBJ:      Contents of section .llvm_link_records:
; OBJ-NEXT: 0000 4c4c5243 010209

!0 = !{i64 16, !"1B", !""}
!1 = !{i64 32, !"1D", !"1B"}

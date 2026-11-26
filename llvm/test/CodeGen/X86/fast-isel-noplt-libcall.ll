; RUN: llc -mtriple=x86_64-unknown-linux-gnu -O0 -fast-isel-abort=1 \
; RUN:   -relocation-model=pic < %s | FileCheck %s
; RUN: llc -mtriple=x86_64-unknown-linux-gnu -O2 -relocation-model=pic < %s | \
; RUN:   FileCheck %s

; With RtLibUseGOT (-fno-plt) FastISel calls a runtime library function
; through the GOT, as SelectionDAG does.

define void @copy(ptr %d, ptr %s, i64 %n) {
; CHECK-LABEL: copy:
; CHECK: {{callq|jmpq}} *memcpy@GOTPCREL(%rip)
  call void @llvm.memcpy.p0.p0.i64(ptr %d, ptr %s, i64 %n, i1 false)
  ret void
}

declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)

!llvm.module.flags = !{!0}
!0 = !{i32 7, !"RtLibUseGOT", i32 1}

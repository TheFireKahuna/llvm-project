; RUN: llc < %s -mtriple=x86_64-pc-windows-msvc | FileCheck %s
; RUN: llc < %s -mtriple=x86_64-unknown-windows-itanium | FileCheck %s
; RUN: llc < %s -mtriple=x86_64-pc-windows-msvc -O0 | FileCheck %s

; With RtLibUseGOT (-fno-plt) a runtime library call on COFF goes through the
; import table, as it goes through the GOT on ELF. Calls to functions the IR
; declares follow their storage class as before.

define void @copy(ptr %d, ptr %s, i64 %n) {
; CHECK-LABEL: copy:
; CHECK: {{callq|jmpq}} *__imp_memcpy(%rip)
  call void @llvm.memcpy.p0.p0.i64(ptr %d, ptr %s, i64 %n, i1 false)
  ret void
}

define i128 @div(i128 %a, i128 %b) {
; CHECK-LABEL: div:
; CHECK: {{callq \*|movq }}__imp___udivti3(%rip)
  %r = udiv i128 %a, %b
  ret i128 %r
}

define void @calls() {
; CHECK-LABEL: calls:
; CHECK: callq local
; CHECK: callq *__imp_imported(%rip)
  call void @local()
  call void @imported()
  ret void
}

; A declaration no front end decided about -- what an optimization or a
; lowering creates -- takes the import form rather than a linker thunk. An
; extern_weak one keeps its stub, since it may resolve to zero.

define void @unmarked_calls() {
; CHECK-LABEL: unmarked_calls:
; CHECK: callq *__imp_unmarked(%rip)
; CHECK: .refptr.weakly(%rip)
  call void @unmarked()
  call void @weakly()
  ret void
}

declare dso_local void @local() nonlazybind
declare dllimport void @imported()
declare void @unmarked()
declare extern_weak void @weakly()
declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)

!llvm.module.flags = !{!0}
!0 = !{i32 7, !"RtLibUseGOT", i32 1}

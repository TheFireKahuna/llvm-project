; RUN: llc < %s -mtriple=x86_64-pc-windows-msvc | \
; RUN:   FileCheck %s --check-prefixes=CHECK,MSVC
; RUN: llc < %s -mtriple=x86_64-unknown-windows-itanium | \
; RUN:   FileCheck %s --check-prefixes=CHECK,ITANIUM
; RUN: llc < %s -mtriple=x86_64-pc-windows-msvc -O0 | \
; RUN:   FileCheck %s --check-prefixes=CHECK,MSVC
; RUN: llc < %s -mtriple=x86_64-w64-windows-gnu | \
; RUN:   FileCheck %s --check-prefixes=CHECK,MSVC

; With RtLibUseGOT (-fno-plt) a runtime library call on Windows Itanium goes
; through the import table, as it goes through the GOT on ELF; MSVC calls it
; directly as before. Calls to functions the IR declares follow their storage
; class.

define void @copy(ptr %d, ptr %s, i64 %n) {
; CHECK-LABEL: copy:
; ITANIUM: {{callq|jmpq}} *__imp_memcpy(%rip)
; MSVC: {{callq|jmp}} memcpy
  call void @llvm.memcpy.p0.p0.i64(ptr %d, ptr %s, i64 %n, i1 false)
  ret void
}

define i128 @div(i128 %a, i128 %b) {
; CHECK-LABEL: div:
; ITANIUM: {{callq \*|movq }}__imp___udivti3(%rip)
; MSVC: callq __udivti3
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

; On Windows Itanium a declaration no front end decided about -- what an
; optimization or a lowering creates -- takes the import form rather than a
; linker thunk. An extern_weak one takes it too: its object keeps it a weak
; external, and the linker binds the import pointer to zero when it is
; absent. MSVC keeps the direct call and the stub.

define void @unmarked_calls() {
; CHECK-LABEL: unmarked_calls:
; ITANIUM: callq *__imp_unmarked(%rip)
; MSVC: callq unmarked
; MSVC: .refptr.weakly(%rip)
; ITANIUM: callq *__imp_weakly(%rip)
  call void @unmarked()
  call void @weakly()
  ret void
}

; A hidden extern_weak declaration can only be defined in the image, so it
; keeps the stub, which the linker never binds to another image's export.

define void @hidden_weak_call() {
; CHECK-LABEL: hidden_weak_call:
; CHECK:       .refptr.hidden_weakly(%rip)
; CHECK-NOT:   __imp_hidden_weakly
  call void @hidden_weakly()
  ret void
}

declare dso_local void @local() nonlazybind
declare dllimport void @imported()
declare void @unmarked()
declare extern_weak void @weakly()
declare extern_weak hidden void @hidden_weakly()
declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)

!llvm.module.flags = !{!0}
!0 = !{i32 7, !"RtLibUseGOT", i32 1}

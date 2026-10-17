; RUN: llc < %s -mtriple=aarch64-pc-windows-msvc | \
; RUN:   FileCheck %s --check-prefixes=CHECK,MSVC
; RUN: llc < %s -mtriple=aarch64-unknown-windows-itanium | \
; RUN:   FileCheck %s --check-prefixes=CHECK,ITANIUM
; RUN: llc < %s -mtriple=aarch64-pc-windows-msvc -global-isel \
; RUN:   -global-isel-abort=2 2>/dev/null | \
; RUN:   FileCheck %s --check-prefixes=CHECK,MSVC
; RUN: llc < %s -mtriple=aarch64-pc-windows-msvc -O0 | \
; RUN:   FileCheck %s --check-prefixes=CHECK,MSVC

; With RtLibUseGOT (-fno-plt) a runtime library call on COFF, which has no
; GOT, goes through the import table, as it goes through the GOT on ELF. Calls
; to functions the IR declares follow their storage class as before.

define void @copy(ptr %d, ptr %s, i64 %n) {
; CHECK-LABEL: copy:
; CHECK:       adrp [[REG:x[0-9]+]], __imp_memcpy
; CHECK-NEXT:  ldr [[REG]], [[[REG]], :lo12:__imp_memcpy]
; CHECK:       {{blr|br}} [[REG]]
  call void @llvm.memcpy.p0.p0.i64(ptr %d, ptr %s, i64 %n, i1 false)
  ret void
}

define void @calls() {
; CHECK-LABEL: calls:
; CHECK:       bl local
; CHECK:       adrp [[REG:x[0-9]+]], __imp_imported
; CHECK-NEXT:  ldr [[REG]], [[[REG]], :lo12:__imp_imported]
; CHECK-NEXT:  blr [[REG]]
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
; ITANIUM:      adrp [[REG:x[0-9]+]], __imp_unmarked
; ITANIUM-NEXT: ldr [[REG]], [[[REG]], :lo12:__imp_unmarked]
; ITANIUM-NEXT: blr [[REG]]
; MSVC:        bl unmarked
; MSVC:        .refptr.weakly
; ITANIUM:     adrp [[REG:x[0-9]+]], __imp_weakly
; ITANIUM-NEXT: ldr [[REG]], [[[REG]], :lo12:__imp_weakly]
; ITANIUM-NEXT: blr [[REG]]
  call void @unmarked()
  call void @weakly()
  ret void
}

; A hidden extern_weak declaration can only be defined in the image, so it
; keeps the stub, which the linker never binds to another image's export.

define void @hidden_weak_call() {
; CHECK-LABEL: hidden_weak_call:
; CHECK:       adrp [[REG:x[0-9]+]], .refptr.hidden_weakly
; CHECK-NEXT:  ldr [[REG]], [[[REG]], :lo12:.refptr.hidden_weakly]
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

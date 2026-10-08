; RUN: llc < %s -mtriple=aarch64-pc-windows-msvc | \
; RUN:   FileCheck %s --check-prefixes=CHECK,MSVC
; RUN: llc < %s -mtriple=aarch64-unknown-windows-itanium | \
; RUN:   FileCheck %s --check-prefixes=CHECK,ITANIUM
; RUN: llc < %s -mtriple=aarch64-pc-windows-msvc -global-isel \
; RUN:   -global-isel-abort=2 2>/dev/null | \
; RUN:   FileCheck %s --check-prefixes=CHECK,MSVC
; RUN: llc < %s -mtriple=aarch64-pc-windows-msvc -O0 | \
; RUN:   FileCheck %s --check-prefixes=CHECK,MSVC

; With RtLibUseGOT (-fno-plt) a runtime library call on COFF goes through the
; import table, as it goes through the GOT on ELF. Calls to functions the IR
; declares follow their storage class as before.

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

; A declaration no front end decided about -- what an optimization or a
; lowering creates -- takes the import form rather than a linker thunk. An
; extern_weak one keeps its stub, since it may resolve to zero, except on
; Windows Itanium, whose objects keep it a weak external and whose linker binds
; the import pointer to zero when it is absent.

define void @unmarked_calls() {
; CHECK-LABEL: unmarked_calls:
; CHECK:       adrp [[REG:x[0-9]+]], __imp_unmarked
; CHECK-NEXT:  ldr [[REG]], [[[REG]], :lo12:__imp_unmarked]
; CHECK-NEXT:  blr [[REG]]
; MSVC:        .refptr.weakly
; ITANIUM:     adrp [[REG:x[0-9]+]], __imp_weakly
; ITANIUM-NEXT: ldr [[REG]], [[[REG]], :lo12:__imp_weakly]
; ITANIUM-NEXT: blr [[REG]]
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

; RUN: llc -mtriple=x86_64-pc-windows-msvc -stop-after=finalize-isel < %s | FileCheck --check-prefix=X64 %s
; RUN: llc -mtriple=i686-pc-windows-msvc -stop-after=finalize-isel < %s | FileCheck --check-prefix=X86 %s
; RUN: %if aarch64-registered-target %{ llc -mtriple=aarch64-pc-windows-msvc -stop-after=finalize-isel < %s | FileCheck --check-prefix=A64 %s %}

; The loader sets an image's _tls_index before any of the image's code runs,
; and it never changes, so the load of it is invariant, and the register
; allocator may load it again rather than spill it.

; X64: MOV32rm $rip, 1, $noreg, &_tls_index, $noreg :: (dereferenceable invariant load (s32))
; X86: MOV32rm $noreg, 1, $noreg, &_tls_index, $noreg :: (dereferenceable invariant load (s32))
; A64: LDRWui %{{[0-9]+}}, target-flags(aarch64-pageoff, aarch64-nc) &_tls_index :: (dereferenceable invariant load (s32))
@v = dso_local thread_local global i32 0

define i32 @f() {
  %p = call ptr @llvm.threadlocal.address.p0(ptr @v)
  %x = load i32, ptr %p
  ret i32 %x
}

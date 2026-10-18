; RUN: llc -mtriple=aarch64-pc-windows-msvc -stop-after=finalize-isel < %s | FileCheck %s

; The loader sets an image's _tls_index before any of the image's code runs,
; and it never changes, so the load of it is invariant, and the register
; allocator may load it again rather than spill it.

@v = dso_local thread_local global i32 0

define i32 @f() {
; CHECK: LDRWui killed %{{[0-9]+}}, target-flags(aarch64-pageoff, aarch64-nc) &_tls_index :: (dereferenceable invariant load (s32))
  %p = call ptr @llvm.threadlocal.address.p0(ptr @v)
  %x = load i32, ptr %p
  ret i32 %x
}

declare ptr @llvm.threadlocal.address.p0(ptr)

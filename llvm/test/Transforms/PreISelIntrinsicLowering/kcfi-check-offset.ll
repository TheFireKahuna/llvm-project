; RUN: not opt -mtriple=x86_64-unknown-linux-gnu -passes=pre-isel-intrinsic-lowering -disable-output < %s 2>&1 | FileCheck %s

;; The generic expansion cannot know the size of a patchable-function prefix
;; between the type word and the entry.

; CHECK: error: a patchable-function prefix is not compatible with llvm.kcfi.check on this target
define void @f(ptr %p) {
  call void @llvm.kcfi.check(ptr %p, i32 1, i32 4)
  call void %p()
  ret void
}

!llvm.module.flags = !{!0}
!0 = !{i32 4, !"kcfi-offset", i32 2}

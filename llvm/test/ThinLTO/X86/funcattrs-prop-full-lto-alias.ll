; RUN: split-file %s %t
; RUN: opt -module-summary %t/full.ll -o %t/full.bc
; RUN: opt -module-summary %t/thin.ll -o %t/thin.bc

;; The index records no resolution for the regular LTO module's summaries, so
;; none prevails. The thin link turns the others available_externally, but not
;; an alias, which keeps its external linkage: propagation goes conservative
;; on it, in either order of the modules.
; RUN: llvm-lto2 run -disable-thinlto-funcattrs=0 %t/full.bc %t/thin.bc \
; RUN:   -o %t1 -save-temps -r=%t/full.bc,f,p -r=%t/full.bc,a,px \
; RUN:   -r=%t/thin.bc,a, -r=%t/thin.bc,g,px
; RUN: llvm-dis %t1.1.3.import.bc -o - | FileCheck %s
; RUN: llvm-lto2 run -disable-thinlto-funcattrs=0 %t/thin.bc %t/full.bc \
; RUN:   -o %t2 -save-temps -r=%t/full.bc,f,p -r=%t/full.bc,a,px \
; RUN:   -r=%t/thin.bc,a, -r=%t/thin.bc,g,px
; RUN: llvm-dis %t2.1.3.import.bc -o - | FileCheck %s

; CHECK: define void @g() [[ATTR:#[0-9]+]]
; CHECK-NOT: attributes [[ATTR]] = { {{.*}}nounwind

;--- full.ll
target triple = "x86_64-unknown-linux-gnu"

@a = alias void (), ptr @f

define void @f() nounwind norecurse {
  ret void
}

!llvm.module.flags = !{!0}
!0 = !{i32 1, !"ThinLTO", i32 0}

;--- thin.ll
target triple = "x86_64-unknown-linux-gnu"

declare void @a()

define void @g() noinline {
  call void @a()
  ret void
}

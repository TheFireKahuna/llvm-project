; RUN: opt -passes=hotcoldsplit -hotcoldsplit-threshold=0 -S < %s | FileCheck %s

; However a function comes by its thread pointer, what is split from it is
; passed that one and does not come by its own again. A function with none
; keeps none.

target triple = "x86_64-pc-windows-ntposix"

declare void @sink() cold

; CHECK-LABEL: define void @probes(
; CHECK:       call void @probes.cold.1()
define void @probes(i1 %c) "thread-pointer-probe" {
entry:
  br i1 %c, label %hot, label %cold
hot:
  ret void
cold:
  call void @sink()
  ret void
}

; CHECK-LABEL: define void @none(
; CHECK:       call void @none.cold.1()
define void @none(i1 %c) "no-thread-pointer" {
entry:
  br i1 %c, label %hot, label %cold
hot:
  ret void
cold:
  call void @sink()
  ret void
}

; CHECK: define internal void @probes.cold.1() #[[PASSED:[0-9]+]]
; CHECK: define internal void @none.cold.1() #[[NONE:[0-9]+]]
; CHECK: attributes #[[PASSED]] = { {{.*}}"thread-pointer-argument" }
; CHECK: attributes #[[NONE]] = { {{.*}}"no-thread-pointer" }

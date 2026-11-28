; RUN: opt -passes=inline -S < %s | FileCheck %s

; A function passed its thread pointer takes its caller's wherever it is
; inlined, and one passed none depends on none. Any other comes by its own,
; which is its caller's only where the caller comes by it the same way.

define void @passed() "thread-pointer-argument" { ret void }
define void @none() "no-thread-pointer" { ret void }
define void @derives() { ret void }
define void @probes() "thread-pointer-probe" { ret void }

; CHECK-LABEL: define void @into_passed()
; CHECK-NOT:   call void @passed()
; CHECK-NOT:   call void @none()
; CHECK:       call void @derives()
; CHECK:       call void @probes()
define void @into_passed() "thread-pointer-argument" {
  call void @passed()
  call void @none()
  call void @derives()
  call void @probes()
  ret void
}

; CHECK-LABEL: define void @into_entry()
; CHECK-NOT:   call void @passed()
; CHECK-NOT:   call void @derives()
; CHECK:       call void @probes()
define void @into_entry() {
  call void @passed()
  call void @derives()
  call void @probes()
  ret void
}

; CHECK-LABEL: define void @into_none()
; CHECK-NOT:   call void @none()
; CHECK:       call void @derives()
define void @into_none() "no-thread-pointer" {
  call void @none()
  call void @derives()
  ret void
}

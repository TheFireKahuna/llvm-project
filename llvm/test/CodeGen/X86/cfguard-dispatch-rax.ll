; RUN: not --crash llc -mtriple=x86_64-pc-windows-msvc -filetype=null < %s 2>&1 | FileCheck %s

; The guard dispatch function takes its target in RAX, so a dispatched call
; that passes anything else there cannot be lowered.

; CHECK: LLVM ERROR: cannot pass the Control Flow Guard target in RAX alone

define void @f(ptr %p) {
  call x86_64_sysvcc void (...) %p(double 1.0) [ "cfguardtarget"(ptr %p) ]
  ret void
}

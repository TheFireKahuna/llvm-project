; RUN: not llvm-as -disable-output %s 2>&1 | FileCheck %s

declare void @g() "thread-pointer-argument"

; CHECK: 'pin-thread-pointer' requires 'thread-pointer-argument'
define void @pin_alone() "pin-thread-pointer" {
  ret void
}

; CHECK: Expected one threadpointer bundle of one pointer operand
; CHECK: Expected one threadpointer bundle of one pointer operand
; CHECK: Expected one threadpointer bundle of one pointer operand
define void @bundles(ptr %p, i64 %i) "thread-pointer-argument" {
  call void @g() [ "threadpointer"(i64 %i) ]
  call void @g() [ "threadpointer"(ptr %p, ptr %p) ]
  call void @g() [ "threadpointer"(ptr %p), "threadpointer"(ptr %p) ]
  ret void
}

; A function that was passed no thread pointer cannot tail call one that is.
; CHECK: cannot guarantee tail call due to mismatched thread pointer
define void @musttail(ptr %p) {
  musttail call void @g() [ "threadpointer"(ptr %p) ]
  ret void
}

; CHECK: a function comes by its thread pointer in one way
define void @two_ways() "thread-pointer-argument" "no-thread-pointer" {
  ret void
}

; CHECK: 'thread-pointer-refusal' requires an integer return type
define void @refusal_void() "thread-pointer-refusal" {
  ret void
}

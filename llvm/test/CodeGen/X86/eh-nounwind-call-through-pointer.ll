; RUN: llc < %s -mtriple=x86_64-linux-gnu | FileCheck %s --check-prefixes=CHECK,X64
; RUN: %if aarch64-registered-target %{ llc < %s -mtriple=aarch64-linux-gnu | FileCheck %s --check-prefixes=CHECK,A64 %}

; A call through the pointer of a nounwind function still calls that function,
; so it needs no call site entry: the only entry covers both invokes.

; CHECK-LABEL: g:
; X64:         callq *%r
; A64:         blr x
; CHECK:       Lcst_begin0:
; CHECK-NEXT:    {{.*}} >> Call Site 1 <<
; CHECK-NOT:     >> Call Site 2 <<
; CHECK:       Lcst_end0:

declare void @f(i32) nounwind nonlazybind
declare void @may()
declare i32 @__gxx_personality_v0(...)

define void @g(i32 %n) personality ptr @__gxx_personality_v0 {
entry:
  invoke void @may() to label %loop unwind label %lp
loop:
  %i = phi i32 [ 0, %entry ], [ %i.next, %loop ]
  call void @f(i32 %i)
  %i.next = add i32 %i, 1
  %c = icmp eq i32 %i.next, %n
  br i1 %c, label %exit, label %loop
exit:
  invoke void @may() to label %ret unwind label %lp
ret:
  ret void
lp:
  %x = landingpad { ptr, i32 } catch ptr null
  ret void
}

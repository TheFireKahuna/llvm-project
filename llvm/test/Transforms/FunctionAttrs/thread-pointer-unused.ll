; RUN: opt -passes=function-attrs -S < %s | FileCheck %s

; A function that is passed its thread pointer holds none, and is
; "thread-pointer-unused", where it never reads it and its calls to readers
; are made less than once per entry: every call it makes is then made at the
; callee's plain entry. A reader is a definition inferred to read it, or a
; declaration that says it does and may touch global memory.

declare ptr @llvm.thread.pointer.p0()
declare ptr @llvm.threadlocal.address.p0(ptr)
declare void @ext() "thread-pointer-argument"
declare void @wanted() "thread-pointer-argument"="read"
declare void @wanted_argmem(ptr) memory(argmem: read) "thread-pointer-argument"="read"
declare void @plain()

@x = thread_local global i32 0

; CHECK: define void @leaf() #[[U:[0-9]+]]
define void @leaf() "thread-pointer-argument" {
  call void @plain()
  ret void
}

; CHECK: define void @even(i32 %n) #[[U2:[0-9]+]]
define void @even(i32 %n) "thread-pointer-argument" {
  call void @odd(i32 %n)
  ret void
}
; CHECK: define void @odd(i32 %n) #[[U2]]
define void @odd(i32 %n) "thread-pointer-argument" {
  call void @even(i32 %n)
  ret void
}

; CHECK: define void @calls_leaf() #[[U]]
define void @calls_leaf() "thread-pointer-argument" {
  call void @leaf()
  ret void
}

; CHECK: define void @reads() #[[R1:[0-9]+]]
define void @reads() "thread-pointer-argument" {
  %tp = call ptr @llvm.thread.pointer.p0()
  ret void
}

; CHECK: define i32 @tls() #[[R2:[0-9]+]]
define i32 @tls() "thread-pointer-argument" {
  %p = call ptr @llvm.threadlocal.address.p0(ptr @x)
  %v = load i32, ptr %p
  ret i32 %v
}

; CHECK: define i32 @calls_tls() #[[R3:[0-9]+]]
define i32 @calls_tls() "thread-pointer-argument" {
  %v = call i32 @tls()
  ret i32 %v
}

; A declaration that does not say it reads it is reached at its plain entry.
; CHECK: define void @external() #[[U]]
define void @external() "thread-pointer-argument" {
  call void @ext()
  ret void
}

; CHECK: define void @wants() #[[R:[0-9]+]]
define void @wants() "thread-pointer-argument" {
  call void @wanted()
  ret void
}

; CHECK: define void @wants_argmem(ptr {{.*}}%p) #[[U3:[0-9]+]]
define void @wants_argmem(ptr %p) "thread-pointer-argument" {
  call void @wanted_argmem(ptr %p)
  ret void
}

; Once per entry in all, from two paths taken half the time each.
; CHECK: define void @wants_halves(i1 %c) #[[R]]
define void @wants_halves(i1 %c) "thread-pointer-argument" {
  br i1 %c, label %a, label %b
a:
  call void @wanted()
  br label %e
b:
  call void @wanted()
  br label %e
e:
  ret void
}

; CHECK: define void @wants_seldom(i1 %c) #[[U]]
define void @wants_seldom(i1 %c) "thread-pointer-argument" {
  br i1 %c, label %a, label %e, !prof !0
a:
  call void @wanted()
  br label %e
e:
  ret void
}

; A function's address is its plain entry, so a call through a pointer
; passes none.
; CHECK: define void @indirect(ptr {{.*}}%f) #[[U]]
define void @indirect(ptr %f) "thread-pointer-argument" {
  call void %f()
  ret void
}

; A definition that may be replaced is inferred too: a call that passes none
; names its plain entry, which derives where the chosen copy reads it.
; CHECK: define linkonce_odr void @odr() #[[U]]
define linkonce_odr void @odr() "thread-pointer-argument" {
  ret void
}

; CHECK: define void @safestack() #[[S:[0-9]+]]
define void @safestack() safestack "thread-pointer-argument" {
  ret void
}

!0 = !{!"branch_weights", i32 1, i32 99}

; CHECK-DAG: attributes #[[U]] = { "thread-pointer-argument" "thread-pointer-unused" }
; CHECK-DAG: attributes #[[U2]] = { {{.*}}"thread-pointer-argument" "thread-pointer-unused" }
; CHECK-DAG: attributes #[[R1]] = { {{.*}}"thread-pointer-argument" }
; CHECK-DAG: attributes #[[R2]] = { {{.*}}"thread-pointer-argument" }
; CHECK-DAG: attributes #[[R3]] = { {{.*}}"thread-pointer-argument" }
; CHECK-DAG: attributes #[[U3]] = { {{.*}}"thread-pointer-argument" "thread-pointer-unused" }
; CHECK-DAG: attributes #[[R]] = { "thread-pointer-argument" }
; CHECK-DAG: attributes #[[S]] = { {{.*}}safestack {{.*}}"thread-pointer-argument" }

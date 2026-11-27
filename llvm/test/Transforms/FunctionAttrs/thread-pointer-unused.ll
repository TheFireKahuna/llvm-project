; RUN: opt -passes=function-attrs -S < %s | FileCheck %s

; A function that is passed its thread pointer and neither reads it nor passes
; it to a function that may is "thread-pointer-unused".

declare ptr @llvm.thread.pointer.p0()
declare ptr @llvm.threadlocal.address.p0(ptr)
declare void @ext() "thread-pointer-argument"
declare void @plain()

@x = thread_local global i32 0

; CHECK: define void @leaf() #[[U1:[0-9]+]]
define void @leaf() "thread-pointer-argument" {
  call void @plain()
  ret void
}

; Recursion in the SCC keeps it.
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

; CHECK: define void @calls_leaf() #[[U1]]
define void @calls_leaf() "thread-pointer-argument" {
  call void @leaf()
  ret void
}

; CHECK: define void @reads() #[[N1:[0-9]+]]
define void @reads() "thread-pointer-argument" {
  %tp = call ptr @llvm.thread.pointer.p0()
  ret void
}

; CHECK: define i32 @tls() #[[N2:[0-9]+]]
define i32 @tls() "thread-pointer-argument" {
  %p = call ptr @llvm.threadlocal.address.p0(ptr @x)
  %v = load i32, ptr %p
  ret i32 %v
}

; CHECK: define void @external() #[[N3:[0-9]+]]
define void @external() "thread-pointer-argument" {
  call void @ext()
  ret void
}

; An indirect call passes one unless its pointer is passed none.
; CHECK: define void @indirect(ptr {{.*}}%f) #[[N3]]
define void @indirect(ptr %f) "thread-pointer-argument" {
  call void %f()
  ret void
}
; CHECK: define void @indirect_none(ptr {{.*}}%f) #[[U1]]
define void @indirect_none(ptr %f) "thread-pointer-argument" {
  call void %f() "no-thread-pointer-argument"
  call win64cc void %f()
  ret void
}

; A copy that may be replaced may call what the chosen one does not.
; CHECK: define linkonce_odr void @odr() #[[N3]]
define linkonce_odr void @odr() "thread-pointer-argument" {
  ret void
}

; CHECK: define void @safestack() #[[N4:[0-9]+]]
define void @safestack() safestack "thread-pointer-argument" {
  ret void
}

; CHECK-DAG: attributes #[[U1]] = { "thread-pointer-argument" "thread-pointer-unused" }
; CHECK-DAG: attributes #[[U2]] = { {{.*}}"thread-pointer-argument" "thread-pointer-unused" }
; CHECK-DAG: attributes #[[N1]] = { {{.*}}"thread-pointer-argument" }
; CHECK-DAG: attributes #[[N2]] = { {{.*}}"thread-pointer-argument" }
; CHECK-DAG: attributes #[[N3]] = { "thread-pointer-argument" }
; CHECK-DAG: attributes #[[N4]] = { {{.*}}safestack {{.*}}"thread-pointer-argument" }

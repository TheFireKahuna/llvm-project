; RUN: opt -mtriple=x86_64-windows-ntposix -passes=thread-pointer -S %s | FileCheck %s
; RUN: opt -mtriple=x86_64-windows-itanium -passes=thread-pointer -S %s | FileCheck %s
; RUN: opt -mtriple=aarch64-windows-ntposix -passes=thread-pointer -S %s | FileCheck %s
; RUN: opt -mtriple=aarch64-windows-itanium -passes=thread-pointer -S %s | FileCheck %s

; A function holds a thread pointer where it reads one or is not inferred
; "thread-pointer-unused", and an entry where it reads one or calls a reader.
; A function that holds one passes it to each callee that may read it, at the
; callee's entry that is passed it; every other call is made at the callee's
; plain entry, which a declaration of the callee's symbol with no "$tp"
; names, as is every call to a callee that touches no global memory; an exact
; definition of the module that holds none is its own plain entry.

declare void @ext() "thread-pointer-argument"
declare void @reader() "thread-pointer-argument"="read"
declare void @argmem(ptr) memory(argmem: read) "thread-pointer-argument"
declare ptr @llvm.thread.pointer.p0()

; A function's address is its plain entry, in data and in code alike; a call
; through a pointer passes no thread pointer. llvm.used keeps the function,
; and its plain entry beside it.
@table = constant [2 x ptr] [ptr @_Z6readerv, ptr @ext]
@llvm.global_ctors = appending global [1 x { i32, ptr, ptr }] [{ i32, ptr, ptr } { i32 65535, ptr @_Z6unusedv, ptr null }]
@llvm.used = appending global [1 x ptr] [ptr @_Z6readerv]
; CHECK-DAG: @table = constant [2 x ptr] [ptr @"\01_Z6readerv", ptr @"\01ext"]
; CHECK-DAG: @llvm.global_ctors = {{.*}}ptr @"\01_Z6unusedv"
; CHECK-DAG: @llvm.used = appending global [2 x ptr] [ptr @_Z6readerv, ptr @"\01_Z6readerv"]

; CHECK-DAG: @"\01unused" = alias void (), ptr @unused
; CHECK-DAG: @"\01_Z6unusedv" = alias void (), ptr @_Z6unusedv

; CHECK-LABEL: define void @holds(
; CHECK-NEXT:  %tp = call ptr @llvm.thread.pointer.p0()
; CHECK-NEXT:  call void @ext() [ "threadpointer"(ptr %tp) ]
; CHECK-NEXT:  call void @reader() [ "threadpointer"(ptr %tp) ]
; CHECK-NEXT:  call void @"\01argmem"(ptr %p)
define void @holds(ptr %p) "thread-pointer-argument" {
  call void @ext()
  call void @reader()
  call void @argmem(ptr %p)
  ret void
}

; CHECK-LABEL: define void @through(
; CHECK-NEXT:  call ptr @llvm.thread.pointer.p0()
; CHECK-NEXT:  store ptr @"\01_Z6readerv", ptr %p
; CHECK-NEXT:  call void %f(){{$}}
define void @through(ptr %p, ptr %f) "thread-pointer-argument" {
  %tp = call ptr @llvm.thread.pointer.p0()
  store ptr @_Z6readerv, ptr %p
  call void %f()
  ret void
}

; CHECK-LABEL: define void @holds_none(
; CHECK-NEXT:  call void @"\01ext"()
; CHECK-NEXT:  call void @"\01reader"()
; CHECK-NEXT:  call void @unused()
define void @holds_none() "thread-pointer-argument" "thread-pointer-unused" {
  call void @ext()
  call void @reader()
  call void @unused()
  ret void
}

; An entry that calls no reader derives nothing.
; CHECK-LABEL: define win64cc void @entry_plain(
; CHECK-NEXT:  call void @"\01ext"()
define win64cc void @entry_plain() {
  call void @ext()
  ret void
}

; CHECK-LABEL: define win64cc void @entry_derives(
; CHECK:       call void @reader() [ "threadpointer"(ptr %{{.*}}) ]
define win64cc void @entry_derives() {
  call void @reader()
  ret void
}

; Plain entries: an alias of a function that holds none, whatever its name;
; a function that calls a reader's entry that is passed it; one for a local
; function only where a function holding none calls it.
; CHECK-LABEL: define void @unused()
define void @unused() "thread-pointer-argument" "thread-pointer-unused" {
  ret void
}
define void @_Z6unusedv() "thread-pointer-argument" "thread-pointer-unused" {
  ret void
}
define void @_Z6readerv() "thread-pointer-argument" {
  %tp = call ptr @llvm.thread.pointer.p0()
  store volatile ptr %tp, ptr null
  ret void
}
define linkonce_odr void @_Z6inlinev() "thread-pointer-argument" comdat {
  %tp = call ptr @llvm.thread.pointer.p0()
  store volatile ptr %tp, ptr null
  ret void
}
$_Z6inlinev = comdat any

define internal void @local_held() "thread-pointer-argument" {
  %tp = call ptr @llvm.thread.pointer.p0()
  store volatile ptr %tp, ptr null
  ret void
}
define internal void @local_plain() "thread-pointer-argument" {
  %tp = call ptr @llvm.thread.pointer.p0()
  store volatile ptr %tp, ptr null
  ret void
}
define void @calls_locals() "thread-pointer-argument" {
  call void @local_held()
  ret void
}
define void @calls_local_plain() "thread-pointer-argument" "thread-pointer-unused" {
  call void @local_plain()
  ret void
}

; CHECK-DAG: define void @"\01_Z6readerv"() #[[PLAIN:[0-9]+]] {{.*}}{
; CHECK-DAG: define linkonce_odr void @"\01_Z6inlinev"() #[[PLAIN]] comdat($_Z6inlinev) {{.*}}{
; CHECK-DAG: define internal void @"\01local_plain"() #[[PLAIN]] {{.*}}{
; CHECK-DAG: declare void @"\01ext"()
; CHECK-DAG: declare void @"\01argmem"(ptr) #[[ARGMEM:[0-9]+]]
; CHECK-NOT: @"\01local_held"
; CHECK-DAG: attributes #[[PLAIN]] = { "thread-pointer-plain-entry" }
; CHECK-DAG: attributes #[[ARGMEM]] = { memory(argmem: read) }

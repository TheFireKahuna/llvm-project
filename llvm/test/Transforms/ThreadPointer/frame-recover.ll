; RUN: opt -mtriple=x86_64-windows-itanium -passes=thread-pointer -S %s | FileCheck %s
; RUN: opt -mtriple=aarch64-windows-ntposix -passes=thread-pointer -S %s | FileCheck %s

; A filter recovers its parent's frame by naming the parent's body, which
; holds the frame, never its plain entry: whether the body holds a thread
; pointer and its plain entry is a function of its own, or holds none and its
; plain entry is an alias of it. A catchpad names a filter that holds none as
; the function it is, its own plain entry. A variadic function, whose plain
; entry would be a second body, keeps its one body, passed none, where a
; filter recovers its frame. A plain entry that is a body of its own unwinds
; through nothing of its own, so its derivation's probe takes a landing-pad
; personality whatever the function's.

; CHECK-DAG:   @"\01unused" = alias i32 (), ptr @unused
; CHECK-NOT:   @"\01vararg"
; CHECK-LABEL: define i32 @holds(
; CHECK:       call void (...) @llvm.localescape(
; CHECK:       catchpad within %{{.*}} [ptr @filt]
; CHECK-LABEL: define internal i32 @filt(
; CHECK:       @llvm.eh.recoverfp(ptr @holds,
; CHECK:       @llvm.localrecover(ptr @holds,
; CHECK:       @llvm.eh.recoverfp(ptr @unused,
; CHECK:       @llvm.localrecover(ptr @unused,
; CHECK:       @llvm.localrecover(ptr @vararg,
; CHECK:       define i32 @vararg(i32 %n, ...) personality
; CHECK:       catchpad within %{{.*}} [ptr @filt]
; CHECK:       define i32 @"\01holds"() #{{[0-9]+}} personality ptr @{{__tp_personality|__gcc_personality_seh0}}
; CHECK-NOT:   @"\01vararg"

@t = thread_local global i32 0

define i32 @holds() "thread-pointer-argument" personality ptr @__C_specific_handler {
entry:
  %b = alloca i32
  call void (...) @llvm.localescape(ptr %b)
  %p = call ptr @llvm.threadlocal.address.p0(ptr @t)
  store i32 1, ptr %p
  invoke void @attach() to label %cont unwind label %dispatch
dispatch:
  %cs = catchswitch within none [label %pad] unwind to caller
pad:
  %cp = catchpad within %cs [ptr @filt]
  catchret from %cp to label %cont
cont:
  %v = load i32, ptr %b
  ret i32 %v
}

define i32 @unused() "thread-pointer-argument" "thread-pointer-unused" personality ptr @__C_specific_handler {
entry:
  %b = alloca i32
  call void (...) @llvm.localescape(ptr %b)
  invoke void @attach() to label %cont unwind label %dispatch
dispatch:
  %cs = catchswitch within none [label %pad] unwind to caller
pad:
  %cp = catchpad within %cs [ptr @filt]
  catchret from %cp to label %cont
cont:
  %v = load i32, ptr %b
  ret i32 %v
}

define internal i32 @filt(ptr %e, ptr %fp) "thread-pointer-argument" "thread-pointer-unused" {
  %f1 = call ptr @llvm.eh.recoverfp(ptr @holds, ptr %fp)
  %b1 = call ptr @llvm.localrecover(ptr @holds, ptr %f1, i32 0)
  %f2 = call ptr @llvm.eh.recoverfp(ptr @unused, ptr %fp)
  %b2 = call ptr @llvm.localrecover(ptr @unused, ptr %f2, i32 0)
  %f3 = call ptr @llvm.eh.recoverfp(ptr @vararg, ptr %fp)
  %b3 = call ptr @llvm.localrecover(ptr @vararg, ptr %f3, i32 0)
  %v1 = load i32, ptr %b1
  %v2 = load i32, ptr %b2
  %v3 = load i32, ptr %b3
  %s = add i32 %v1, %v2
  %r = add i32 %s, %v3
  ret i32 %r
}

define i32 @vararg(i32 %n, ...) "thread-pointer-argument" personality ptr @__C_specific_handler {
entry:
  %b = alloca i32
  call void (...) @llvm.localescape(ptr %b)
  %p = call ptr @llvm.threadlocal.address.p0(ptr @t)
  store i32 %n, ptr %p
  invoke void @attach() to label %cont unwind label %dispatch
dispatch:
  %cs = catchswitch within none [label %pad] unwind to caller
pad:
  %cp = catchpad within %cs [ptr @filt]
  catchret from %cp to label %cont
cont:
  %v = load i32, ptr %b
  ret i32 %v
}

declare void @attach()
declare i32 @__C_specific_handler(...)
declare void @llvm.localescape(...)
declare ptr @llvm.localrecover(ptr, ptr, i32 immarg)
declare ptr @llvm.eh.recoverfp(ptr, ptr)
declare ptr @llvm.threadlocal.address.p0(ptr)

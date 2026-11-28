; RUN: opt -mtriple=x86_64-unknown-windows-itanium -passes=win-eh-prepare -S < %s | FileCheck %s
; RUN: opt -mtriple=aarch64-unknown-windows-itanium -passes=win-eh-prepare -S < %s | FileCheck %s
; RUN: not opt -mtriple=x86_64-unknown-windows-itanium -passes=win-eh-prepare -S -o /dev/null %S/Inputs/async-eh-scopes-crossed.ll 2>&1 | FileCheck %s --check-prefix=CROSSED

; A function that names __gxx_personality_seha0 has exception tables that
; cover every instruction of an EH scope. WinEHPrepare finds the scope each
; block is in from the llvm.seh.* markers and gives the block an invoke of
; llvm.seh.scope.begin of its own that unwinds to the scope's pad.

; CROSSED: error: in function 'crossed': block 'both' is in two exception handling scopes, neither inside the other

declare void @may_throw()
declare void @dtor(ptr) nounwind
declare i32 @__gxx_personality_seha0(...)
declare void @llvm.seh.scope.begin()
declare void @llvm.seh.scope.end()
declare void @llvm.seh.try.begin()
declare void @llvm.seh.try.end()
@_ZTIi = external constant ptr

; A scope opened on one path to a block is open in the block: its cleanup
; tests whether there is anything to clean up. The block that did not open it
; is outside.
define void @conditional(i1 %c, ptr %p) personality ptr @__gxx_personality_seha0 {
; CHECK-LABEL: define void @conditional(
; CHECK:       then:
; CHECK-NEXT:    store i1 true, ptr %active
; CHECK-NEXT:    br label %join
; CHECK:       else:
; CHECK-NEXT:    store i32 1, ptr %p
; CHECK-NEXT:    br label %join
; CHECK:       join:
; CHECK-NEXT:    invoke void @llvm.seh.scope.begin()
; CHECK-NEXT:      to label %[[JOIN:.*]] unwind label %cleanup
; CHECK:       [[JOIN]]:
; CHECK-NEXT:    store i32 2, ptr %p
entry:
  %active = alloca i1
  store i1 false, ptr %active
  br i1 %c, label %then, label %else
then:
  store i1 true, ptr %active
  invoke void @llvm.seh.scope.begin() to label %join unwind label %cleanup
else:
  store i32 1, ptr %p
  br label %join
join:
  store i32 2, ptr %p
  invoke void @llvm.seh.scope.end() to label %out unwind label %cleanup
out:
  ret void
cleanup:
  %pad = cleanuppad within none []
  %is.active = load i1, ptr %active
  br i1 %is.active, label %destroy, label %done
destroy:
  call void @dtor(ptr %p) [ "funclet"(token %pad) ]
  br label %done
done:
  cleanupret from %pad unwind to caller
}

; A try block inside a cleanup's scope: its body is in the try block's scope,
; whose pad is its catchswitch, and its handler in the cleanup's.
define void @nested(ptr %p) personality ptr @__gxx_personality_seha0 {
; CHECK-LABEL: define void @nested(
; CHECK:       {{^}}try:
; CHECK-NEXT:    invoke void @llvm.seh.scope.begin()
; CHECK-NEXT:      to label %[[TRY:.*]] unwind label %cs
; CHECK:       [[TRY]]:
; CHECK-NEXT:    store i32 1, ptr %p
; CHECK:       handler:
; CHECK-NEXT:    invoke void @llvm.seh.scope.begin()
; CHECK-NEXT:      to label %[[CATCH:.*]] unwind label %cleanup
; CHECK:       [[CATCH]]:
; CHECK-NEXT:    store i32 2, ptr %p
entry:
  invoke void @llvm.seh.scope.begin() to label %outer unwind label %cleanup
outer:
  invoke void @llvm.seh.try.begin() to label %try unwind label %cs
try:
  store i32 1, ptr %p
  invoke void @llvm.seh.try.end() to label %cont unwind label %cs
cs:
  %s = catchswitch within none [label %catch] unwind label %cleanup
catch:
  %cp = catchpad within %s [ptr null]
  catchret from %cp to label %handler
handler:
  store i32 2, ptr %p
  br label %cont
cont:
  invoke void @llvm.seh.scope.end() to label %out unwind label %cleanup
out:
  ret void
cleanup:
  %pad = cleanuppad within none []
  call void @dtor(ptr %p) [ "funclet"(token %pad) ]
  cleanupret from %pad unwind to caller
}

; A handler of an inner try block is in the scope of the outer one, and the
; outer try block's handler is behind it, whichever handler's code reaches it.
define void @handlers(i1 %c, ptr %p) personality ptr @__gxx_personality_seha0 {
; CHECK-LABEL: define void @handlers(
; CHECK:       handler.inner:
; CHECK-NEXT:    invoke void @llvm.seh.scope.begin()
; CHECK-NEXT:      to label %[[INNER:.*]] unwind label %cs.outer
; CHECK:       [[INNER]]:
; CHECK-NEXT:    store i32 2, ptr %p
; CHECK-NEXT:    br i1 %c, label %cont, label %handler
; CHECK:       {{^}}handler:
; CHECK-NEXT:    store i32 3, ptr %p
entry:
  invoke void @llvm.seh.try.begin() to label %outer unwind label %cs.outer
outer:
  invoke void @llvm.seh.try.begin() to label %inner unwind label %cs.inner
inner:
  store i32 1, ptr %p
  invoke void @llvm.seh.try.end() to label %cont unwind label %cs.inner
cont:
  invoke void @llvm.seh.try.end() to label %out unwind label %cs.outer
cs.inner:
  %si = catchswitch within none [label %catch.inner] unwind label %cs.outer
catch.inner:
  %ci = catchpad within %si [ptr @_ZTIi]
  catchret from %ci to label %handler.inner
handler.inner:
  store i32 2, ptr %p
  br i1 %c, label %cont, label %handler
cs.outer:
  %so = catchswitch within none [label %catch.outer] unwind to caller
catch.outer:
  %co = catchpad within %so [ptr null]
  catchret from %co to label %handler
handler:
  store i32 3, ptr %p
  br label %out
out:
  ret void
}

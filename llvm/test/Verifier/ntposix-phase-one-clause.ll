; RUN: split-file %s %t
; RUN: not opt -passes=verify -disable-output %t/missing.ll 2>&1 | FileCheck %s --check-prefix=MISSING
; RUN: not opt -passes=verify -disable-output %t/joins.ll 2>&1 | FileCheck %s --check-prefix=JOINS
; RUN: not opt -passes=verify -disable-output %t/resumes.ll 2>&1 | FileCheck %s --check-prefix=RESUMES
; RUN: opt -passes=verify -disable-output %t/valid.ll
; RUN: not llc -disable-verify -mtriple=x86_64-pc-windows-ntposix -filetype=null %t/missing.ll 2>&1 | FileCheck %s --check-prefix=PREPARE

; On NT-POSIX every cleanuppad carries its sites' phase-one clause as one
; `i8`, a cleanupret joins only pads a search treats alike, and a pad a
; search does not pass never resumes to the caller.

;--- missing.ll
target triple = "x86_64-pc-windows-ntposix"
declare void @may_throw()
declare i32 @rust_eh_personality(...)
; MISSING: NT-POSIX cleanup funclet breaks its phase-one clause
; MISSING-NEXT: %c = cleanuppad within none []
; PREPARE: NT-POSIX cleanup funclet in 'f' breaks its phase-one clause
define void @f() personality ptr @rust_eh_personality {
entry:
  invoke void @may_throw() to label %done unwind label %cleanup
done:
  ret void
cleanup:
  %c = cleanuppad within none []
  cleanupret from %c unwind to caller
}

;--- joins.ll
target triple = "x86_64-pc-windows-ntposix"
declare void @may_throw()
declare void @abort() nounwind
declare i32 @rust_eh_personality(...)
; JOINS: NT-POSIX cleanup funclet breaks its phase-one clause
; JOINS-NEXT: cleanupret from %c unwind label %terminate
define void @f() personality ptr @rust_eh_personality {
entry:
  invoke void @may_throw() to label %done unwind label %cleanup
done:
  ret void
cleanup:
  %c = cleanuppad within none [i8 0]
  cleanupret from %c unwind label %terminate
terminate:
  %t = cleanuppad within none [i8 2]
  call void @abort() [ "funclet"(token %t) ]
  unreachable
}

;--- resumes.ll
target triple = "x86_64-pc-windows-ntposix"
declare void @may_throw()
declare i32 @rust_eh_personality(...)
; RESUMES: NT-POSIX cleanup funclet breaks its phase-one clause
; RESUMES-NEXT: cleanupret from %c unwind to caller
define void @f() personality ptr @rust_eh_personality {
entry:
  invoke void @may_throw() to label %done unwind label %cleanup
done:
  ret void
cleanup:
  %c = cleanuppad within none [i8 1]
  cleanupret from %c unwind to caller
}

;--- valid.ll
target triple = "x86_64-pc-windows-ntposix"
declare void @may_throw()
declare void @abort() nounwind
declare i32 @rust_eh_personality(...)
define void @f() personality ptr @rust_eh_personality {
entry:
  invoke void @may_throw() to label %mid unwind label %outer
mid:
  invoke void @may_throw() to label %done unwind label %inner
done:
  ret void
inner:
  %i = cleanuppad within none [i8 0]
  cleanupret from %i unwind label %outer
outer:
  %o = cleanuppad within none [i8 0]
  cleanupret from %o unwind to caller
}
define void @g() nounwind personality ptr @rust_eh_personality {
entry:
  invoke void @may_throw() to label %done unwind label %cleanup
done:
  ret void
cleanup:
  %c = cleanuppad within none [i8 1]
  cleanupret from %c unwind label %terminate
terminate:
  %t = cleanuppad within none [i8 2]
  call void @abort() [ "funclet"(token %t) ]
  unreachable
}

; RUN: llc -mtriple=x86_64-windows-itanium -verify-machineinstrs < %s | FileCheck %s
; RUN: %if aarch64-registered-target %{ llc -mtriple=aarch64-windows-itanium \
; RUN:   -verify-machineinstrs < %s | FileCheck %s %}

; A personality the module defines, passed its thread pointer, is named by
; its plain entry, which the unwinder calls, and is still the personality it
; is by that name: the catchpad takes its exception pointer in its register.

; CHECK-LABEL: f$tp:
; CHECK:       .seh_handler __gxx_personality_seh0, @unwind, @except
; CHECK:       %catch
; CHECK:       __cxa_begin_catch

define void @f() "thread-pointer-argument" personality ptr @__gxx_personality_seh0 {
entry:
  invoke void @g() to label %done unwind label %dispatch
dispatch:
  %cs = catchswitch within none [label %catch] unwind to caller
catch:
  %cp = catchpad within %cs [ptr null]
  %e = call ptr @llvm.eh.exceptionpointer.p0(token %cp)
  catchret from %cp to label %body
body:
  call ptr @__cxa_begin_catch(ptr %e)
  ret void
done:
  ret void
}

define i32 @__gxx_personality_seh0(ptr, ptr, ptr, ptr) "thread-pointer-argument" {
  ret i32 1
}

declare void @g() "thread-pointer-argument"
declare ptr @__cxa_begin_catch(ptr) "thread-pointer-argument"
declare ptr @llvm.eh.exceptionpointer.p0(token)

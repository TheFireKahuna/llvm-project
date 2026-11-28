; RUN: llc -mtriple=x86_64-windows-msvc < %s | FileCheck %s
; RUN: llc -mtriple=x86_64-windows-msvc -filetype=obj < %s -o /dev/null

; Only an SEH personality's table defines a parent's frame offset. A filter
; recovering the frame of a parent whose personality keeps no such table,
; whose exceptional code no SEH table describes, recovers the frame pointer it
; was given, as for a parent with no personality.

; CHECK-LABEL: filt:
; CHECK-NOT:   parent_frame_offset
; CHECK:       movl .Lparent$frame_escape_0(%rdx), %eax
; CHECK:       retq

define i32 @filt(ptr %e, ptr %fp) {
  %f = call ptr @llvm.eh.recoverfp(ptr @parent, ptr %fp)
  %b = call ptr @llvm.localrecover(ptr @parent, ptr %f, i32 0)
  %v = load i32, ptr %b
  ret i32 %v
}

define void @parent() personality ptr @__gxx_personality_seh0 {
  %b = alloca i32
  call void (...) @llvm.localescape(ptr %b)
  invoke void @g() to label %ret unwind label %lpad
lpad:
  %l = landingpad { ptr, i32 } cleanup
  resume { ptr, i32 } %l
ret:
  ret void
}

declare void @g()
declare i32 @__gxx_personality_seh0(...)
declare void @llvm.localescape(...)
declare ptr @llvm.localrecover(ptr, ptr, i32 immarg)
declare ptr @llvm.eh.recoverfp(ptr, ptr)

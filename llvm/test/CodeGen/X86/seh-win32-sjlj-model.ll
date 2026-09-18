; SEH __try functions carry an MSVC personality and funclet EH regardless of
; the module's exception model. Two things used to go wrong when that model was
; not WinEH: with -exception-model=sjlj, SjLjEHPrepare treated the catchswitch
; block as a landing pad and crashed on the null LandingPadInst, and under any
; non-WinEH model the AsmPrinter had no WinException streamer, so the
; __except_handler3 registration referenced an L__ehtable$ label that was never
; emitted. Functions with an SjLj personality in the same module must still be
; lowered by SjLjEHPrepare.
;
; RUN: llc -mtriple=i686-pc-windows-gnu -exception-model=sjlj < %s | FileCheck %s --check-prefixes=CHECK,SJLJ
; RUN: llc -mtriple=i686-pc-windows-gnu < %s | FileCheck %s --check-prefixes=CHECK,DWARF
; RUN: llc -mtriple=i686-unknown-windows-itanium -exception-model=sjlj < %s | FileCheck %s --check-prefixes=CHECK,SJLJ

declare void @g()
declare void @h(ptr)
declare i32 @_except_handler3(...)
declare i32 @__gxx_personality_sj0(...)
declare void @llvm.localescape(...)
declare ptr @llvm.frameaddress.p0(i32 immarg)
declare ptr @llvm.eh.recoverfp(ptr, ptr)
declare ptr @llvm.localrecover(ptr, ptr, i32 immarg)

define i32 @seh_try() personality ptr @_except_handler3 {
entry:
  %code = alloca i32, align 4
  call void (...) @llvm.localescape(ptr %code)
  invoke void @g()
          to label %return unwind label %catch.dispatch

catch.dispatch:
  %cs = catchswitch within none [label %except] unwind to caller

except:
  %cp = catchpad within %cs [ptr @filter]
  catchret from %cp to label %return

return:
  %result = phi i32 [ 1, %except ], [ 0, %entry ]
  ret i32 %result
}

define internal i32 @filter() {
entry:
  %fp = call ptr @llvm.frameaddress.p0(i32 1)
  %parentfp = call ptr @llvm.eh.recoverfp(ptr @seh_try, ptr %fp)
  %code = call ptr @llvm.localrecover(ptr @seh_try, ptr %parentfp, i32 0)
  store i32 1, ptr %code, align 4
  ret i32 1
}

; The __try function is lowered through the Win32 EH state machinery and its
; handler table is emitted under either model.
; CHECK-LABEL: _seh_try:
; CHECK: movl $L__ehtable$seh_try,
; CHECK: movl $__except_handler3,
; CHECK: movl %fs:0,
; CHECK-NOT: _Unwind_SjLj_Register
; CHECK: .section .xdata
; CHECK: L__ehtable$seh_try:
; CHECK-NEXT: .long -1
; CHECK-NEXT: .long _filter
; CHECK-NEXT: .long LBB0_{{[0-9]+}}

define void @sjlj_cleanup(ptr %obj) personality ptr @__gxx_personality_sj0 {
entry:
  invoke void @g()
          to label %done unwind label %lpad

lpad:
  %lp = landingpad { ptr, i32 }
          cleanup
  call void @h(ptr %obj)
  resume { ptr, i32 } %lp

done:
  ret void
}

; The SjLj-personality function is still prepared by SjLjEHPrepare, and the
; DWARF-model build uses ordinary landing pads.
; CHECK-LABEL: _sjlj_cleanup:
; SJLJ: __Unwind_SjLj_Register
; SJLJ: __Unwind_SjLj_Unregister
; DWARF-NOT: _Unwind_SjLj_Register
; DWARF: __Unwind_Resume

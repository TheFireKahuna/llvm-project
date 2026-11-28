; DEFINE: %{llc} = llc -function-splitting=all -mfs-split-ehcode
; RUN: %{llc} -mtriple=x86_64-unknown-windows-itanium -verify-machineinstrs < %s \
; RUN:   | FileCheck %s --check-prefixes=CHECK,X64
; RUN: %{llc} -mtriple=x86_64-unknown-windows-itanium -O0 \
; RUN:   -x86-wineh-unwindv2-instruction-count-threshold=4 < %s \
; RUN:   | FileCheck %s --check-prefix=CUT
; RUN: %if aarch64-registered-target %{ %{llc} -mtriple=aarch64-unknown-windows-itanium -verify-machineinstrs < %s \
; RUN:   | FileCheck %s --check-prefixes=CHECK,A64 %}
; DEFINE: %{rule} = FileCheck %s --implicit-check-not=trap.cold \
; DEFINE:   --implicit-check-not=loop.cold
; RUN: llc -mtriple=x86_64-unknown-windows-itanium -verify-machineinstrs < %s \
; RUN:   | %{rule} --check-prefixes=RULE,RX64
; RUN: %if aarch64-registered-target %{ llc -mtriple=aarch64-unknown-windows-itanium -verify-machineinstrs < %s \
; RUN:   | %{rule} --check-prefixes=RULE,RA64 %}

; A function whose exception handling code lies in its cold section,
; .text$split, is two functions to the unwinder: each part has unwind info and
; an exception table of its own.

declare void @may_throw()
declare void @g(i32)
declare void @fail(i32) noreturn
declare void @abort() noreturn nounwind
declare void @llvm.trap()
declare ptr @__cxa_begin_catch(ptr) nounwind
declare void @__cxa_end_catch()
declare i32 @__gxx_personality_seh0(...)
declare ptr @llvm.eh.exceptionpointer.p0(token)

; No jump follows a call that does not return into the unreachable code behind
; it. A handler that has a handler of its own is in the cold part's table,
; which is complete: its own scope, call site, action and type, the pad from
; its own start.
; CHECK-LABEL: throws:
; X64:         callq fail
; X64-NOT:     jmp
; X64:         int3
; X64-NEXT:  .LBB_END0_{{[0-9]+}}:
; X64-NEXT:    .seh_handlerdata
; A64:         bl fail
; A64-NOT:     b throws.cold
; A64:         .seh_handlerdata
; X64:         .long throws.cold-.Llpstart
; A64:         .word throws.cold-.Llpstart
; CHECK:     throws.cold:
; CHECK-NEXT: .seh_proc throws
; CHECK:       .seh_handler __gxx_personality_seh0, @unwind, @except
; CHECK-NEXT:  nop
; CHECK-NEXT: [[CATCH:.LBB0_[0-9]+]]: {{.*}} %catch
; CHECK:      [[SITE:.Ltmp[0-9]+]]:
; X64-NEXT:    callq __cxa_end_catch
; A64-NEXT:    bl __cxa_end_catch
; CHECK:      [[CATCH2:.LBB0_[0-9]+]]: {{.*}} %catch2
; CHECK:       .seh_handlerdata
; CHECK:       .byte 255 {{.*}} Cleanup base Encoding = omit
; CHECK-NEXT:  .byte 1 {{.*}} Scopes
; CHECK-NEXT:  .byte 0
; CHECK-NEXT:  .byte 1
; CHECK-NEXT:  .byte 1
; CHECK-NEXT:  .uleb128 [[CATCH2]]-throws.cold
; CHECK-NEXT:  .uleb128 [[SITE]]-throws.cold
; CHECK-NEXT:  .byte 1
; CHECK:       .byte 255 {{.*}} @LPStart Encoding = omit
; CHECK-NEXT:  .byte 155 {{.*}} @TType Encoding = indirect pcrel sdata4
; CHECK:       .uleb128 [[SITE]]-throws.cold {{.*}} Call Site 1
; CHECK-NEXT:  .uleb128
; CHECK-NEXT:  .uleb128 [[CATCH2]]-throws.cold
; CHECK-NEXT:  .byte 1 {{.*}} On action: 1
; CHECK:       .byte 1 {{.*}} Action Record 1
; CHECK:       {{.long|.word}} 0 {{.*}} TypeInfo 1
define i32 @throws(i32 %x) uwtable personality ptr @__gxx_personality_seh0 {
entry:
  invoke void @fail(i32 %x) to label %unreachable unwind label %cs
cs:
  %s = catchswitch within none [label %catch] unwind to caller
catch:
  %p = catchpad within %s [ptr null]
  %e = call ptr @llvm.eh.exceptionpointer.p0(token %p)
  %b = call ptr @__cxa_begin_catch(ptr %e) [ "funclet"(token %p) ]
  invoke void @__cxa_end_catch() [ "funclet"(token %p) ] to label %caught unwind label %cs2
caught:
  catchret from %p to label %done
cs2:
  %s2 = catchswitch within %p [label %catch2] unwind to caller
catch2:
  %p2 = catchpad within %s2 [ptr null]
  catchret from %p2 to label %done
done:
  ret i32 %x
unreachable:
  unreachable
}

; Unwind info that an epilogue lies too far from the end of is followed by a
; part in its own section with unwind info and a table of its own, not
; chained info. A part none of whose calls has a landing pad has neither
; handler nor table.
; CUT-LABEL: cut:
; CUT-NOT:     .seh_handler
; CUT:         jne cut.__part.1
; CUT:         jmp g2 # TAILCALL
; CUT-NEXT:  .LBB_END1_{{[0-9]+}}:
; CUT-NEXT:    .seh_endproc
; CUT-NEXT:  cut.__part.1:
; CUT-NEXT:  .seh_proc cut
; CUT-NEXT:    .seh_unwindversion 2
; CUT-NEXT:    .seh_stackalloc 40
; CUT-NEXT:    .seh_endprologue
; CUT-NEXT:    .seh_handler __gxx_personality_seh0, @unwind, @except
; CUT:         .seh_handlerdata
; CUT:         .uleb128 [[SITE:.Ltmp[0-9]+]]-cut.__part.1
; CUT:         .long cut.cold-.Llpstart
; CUT:         .uleb128 [[SITE]]-cut.__part.1 # >> Call Site 1 <<
; CUT:         .seh_endproc
; CUT-NEXT:    .section .text$split,"xr"
; CUT-NEXT:  cut.cold:
; CUT-NOT:     .seh_splitchained
define i32 @cut(i32 %x) uwtable personality ptr @__gxx_personality_seh0 {
entry:
  %c = icmp eq i32 %x, 7
  br i1 %c, label %a, label %b
a:
  %r = tail call i32 @g2(i32 1)
  ret i32 %r
b:
  invoke void @may_throw() to label %cont unwind label %cs
cs:
  %s = catchswitch within none [label %catch] unwind to caller
catch:
  %p = catchpad within %s [ptr null]
  catchret from %p to label %cont
cont:
  %y = call i32 @g2(i32 2)
  %z = call i32 @g2(i32 %y)
  ret i32 %z
}
declare i32 @g2(i32)

; A funclet's unwind info is cut the same way: the part repeats the funclet's
; prologue codes and, like the funclet, has no handler.
; CUT-LABEL: "?dtor$2@?0?cutcleanup@4HA":
; CUT-NEXT:  .seh_proc "?dtor$2@?0?cutcleanup@4HA"
; CUT:         je cutcleanup.__part.1
; CUT:         jmp may_throw # TAILCALL
; CUT-NEXT:  .LBB_END{{[0-9_]+}}:
; CUT-NEXT:    .seh_endproc
; CUT-NEXT:  cutcleanup.__part.1:
; CUT-NEXT:  .seh_proc cutcleanup
; CUT-NEXT:    .seh_unwindversion 2
; CUT-NEXT:    .seh_stackalloc 40
; CUT-NEXT:    .seh_endprologue
; CUT-NEXT:    callq may_throw
; CUT-NOT:     .seh_handler
; CUT:         .seh_endproc
define void @cutcleanup(i1 %c) uwtable personality ptr @__gxx_personality_seh0 {
entry:
  invoke void @may_throw() to label %done unwind label %cleanup
cleanup:
  %cp = cleanuppad within none []
  br i1 %c, label %l, label %r
l:
  call void @may_throw() [ "funclet"(token %cp) ]
  cleanupret from %cp unwind to caller
r:
  call void @may_throw() [ "funclet"(token %cp) ]
  call void @may_throw() [ "funclet"(token %cp) ]
  call void @may_throw() [ "funclet"(token %cp) ]
  cleanupret from %cp unwind to caller
done:
  ret void
}


; Windows Itanium and NT-POSIX split every function with no option and no
; profile: the code that the static branch probabilities make cold lies in the
; function's cold section with its handlers and cleanups, where the function's
; own section shrinks by it.

; A path that ends in a call that does not return moves. The cold part's
; unwind info states the prologue's unwind codes with no instruction, it has
; no handler where the function has none, and x86-64's ends past a call that
; is its last instruction.
; RULE-LABEL: noreturn:
; RX64:        .seh_unwindversion 2
; RX64:        je noreturn.cold
; RA64:        b.eq [[ISLAND:.LBB[0-9_]+]]
; RA64:      [[ISLAND]]:
; RA64:        b noreturn.cold
; RULE:        .seh_endproc
; RULE-NEXT:   .section .text$split,"xr"
; RULE-NEXT: noreturn.cold:
; RULE-NEXT: .seh_proc noreturn
; RX64-NEXT:   .seh_unwindversion 2
; RX64-NEXT:   .seh_pushreg %rsi
; RX64-NEXT:   .seh_stackalloc 32
; RA64-NEXT:   .seh_stackalloc 16
; RA64-NEXT:   .seh_save_lrpair x19, 0
; RULE-NEXT:   .seh_endprologue
; RX64:        callq abort
; RX64-NEXT:   int3
; RA64:        bl abort
; RULE-NOT:    .seh_handler
; RULE:        .seh_endproc
define i32 @noreturn(i32 %x) uwtable {
entry:
  %c = icmp eq i32 %x, 7
  br i1 %c, label %die, label %ok
die:
  call void @g(i32 %x)
  call void @g(i32 2)
  call void @g(i32 3)
  call void @abort()
  unreachable
ok:
  call void @g(i32 1)
  ret i32 %x
}

; A path that the source calls unlikely moves, and jumps back.
; RULE-LABEL: unlikely:
; RX64:      [[OK:.LBB[0-9_]+]]: # %ok
; RA64:      [[OK:.LBB[0-9_]+]]: // %ok
; RULE:      unlikely.cold:
; RX64:        jmp [[OK]]
; RA64:        b [[OK]]
define i32 @unlikely(i32 %x) uwtable {
entry:
  %c = icmp eq i32 %x, 7
  br i1 %c, label %rare, label %ok, !prof !0
rare:
  call void @g(i32 %x)
  call void @g(i32 2)
  call void @g(i32 3)
  call void @g(i32 4)
  br label %ok
ok:
  call void @g(i32 1)
  ret i32 %x
}

; A block that calls a function the source marks cold moves, with the block
; whose every path leads to it, though that path returns to the hot code.
; RULE-LABEL: cold_call:
; RX64:        je cold_call.cold
; RA64:        b cold_call.cold
; RULE:      cold_call.cold: {{.*}} %before
; RX64:        callq report
; RA64:        bl report
define i32 @cold_call(i32 %x) uwtable {
entry:
  %c = icmp eq i32 %x, 7
  br i1 %c, label %before, label %ok
before:
  call void @g(i32 %x)
  br label %rare
rare:
  call void @report(i32 2)
  br label %ok
ok:
  call void @g(i32 1)
  ret i32 %x
}
declare void @report(i32) cold

; A trap is no longer than what the branch to it would grow by, and a
; function none of whose code moves is as it was.
; RULE-LABEL: trap:
; RX64:        ud2
; RX64-NEXT: .Lfunc_end{{[0-9]+}}:
; RA64:        brk #0x1
; RA64-NEXT: .Lfunc_end{{[0-9]+}}:
define i32 @trap(i32 %x) uwtable {
entry:
  %c = icmp eq i32 %x, 7
  br i1 %c, label %die, label %ok
die:
  call void @llvm.trap()
  unreachable
ok:
  call void @g(i32 1)
  ret i32 %x
}

; A loop's exit is the least probable edge of normal operation, and is hot.
; RULE-LABEL: loop:
define i32 @loop(i32 %n) uwtable {
entry:
  br label %head
head:
  %i = phi i32 [ 0, %entry ], [ %j, %head ]
  call void @g(i32 %i)
  %j = add i32 %i, 1
  %c = icmp eq i32 %j, %n
  br i1 %c, label %exit, label %head
exit:
  call void @g(i32 -1)
  call void @g(i32 -2)
  call void @g(i32 -3)
  call void @g(i32 -4)
  ret i32 %n
}

; A function's handlers lie in one section, where the table's pads count from
; one start, though one of them leads to a cleanup and the other to none: both
; move, ahead of the cleanup.
; RULE-LABEL: handlers:
; RULE:        .section .text$split,"xr"
; RULE-NEXT: handlers.cold:
; RULE:        {{#|//}} %plain
; RULE:        {{#|//}} %guarded
; RULE:      "?dtor$
; RULE:      .Lfunc_end
define i32 @handlers(i32 %x) uwtable personality ptr @__gxx_personality_seh0 {
entry:
  invoke void @may_throw() to label %ok unwind label %cs
cs:
  %s = catchswitch within none [label %plain, label %guarded] unwind to caller
plain:
  %p1 = catchpad within %s [ptr @_ZTIi]
  catchret from %p1 to label %ok
guarded:
  %p2 = catchpad within %s [ptr null]
  %e = call ptr @llvm.eh.exceptionpointer.p0(token %p2)
  %b = call ptr @__cxa_begin_catch(ptr %e) [ "funclet"(token %p2) ]
  invoke void @may_throw() [ "funclet"(token %p2) ] to label %done unwind label %cleanup
done:
  call void @__cxa_end_catch() [ "funclet"(token %p2) ]
  catchret from %p2 to label %ok
cleanup:
  %cp = cleanuppad within %p2 []
  call void @__cxa_end_catch() [ "funclet"(token %cp) ]
  cleanupret from %cp unwind to caller
ok:
  ret i32 %x
}
@_ZTIi = external constant ptr

; A function with no frame has parts with no unwind info, which cost none,
; and the parts of the function after it are that function's. A frame that
; only the cold code needs begins there, prologue and all, and is the
; function's one unwind range, whose epilogue on x86-64 jumps back.
; RULE-LABEL: frameless:
; RULE-NOT:    .seh_
; RULE:      frameless.cold:
; RULE-NOT:    .seh_
; RULE-LABEL: cold_frame:
; RULE-NOT:    .seh_
; RX64:      [[OUT:.LBB[0-9_]+]]: # %out
; RULE-NOT:    .seh_
; RULE:        .section .text$split,"xr"
; RULE-NEXT: .seh_proc cold_frame
; RULE-NEXT: cold_frame.cold:
; RX64-NEXT:   .seh_unwindversion 2
; RX64-NEXT:   subq $40, %rsp
; RA64-NEXT:   str x30, [sp, #-16]!
; RULE:        .seh_endprologue
; RX64:        .seh_endepilogue
; RX64-NEXT:   jmp [[OUT]]
; RULE:        .seh_endproc
define i32 @frameless(i32 %x) uwtable {
entry:
  %c = icmp eq i32 %x, 7
  br i1 %c, label %rare, label %ok, !prof !0
rare:
  %a = add i32 %x, 1
  %r = tail call i32 @g2(i32 %a)
  ret i32 %r
ok:
  ret i32 %x
}

define i32 @cold_frame(i32 %x, ptr %p) uwtable {
entry:
  %c = icmp eq i32 %x, 7
  br i1 %c, label %rare, label %out, !prof !0
rare:
  call void @g(i32 1)
  call void @g(i32 2)
  br label %out
out:
  %q = load ptr, ptr %p
  %v = load i32, ptr %q
  ret i32 %v
}

!0 = !{!"branch_weights", i32 1, i32 2000}

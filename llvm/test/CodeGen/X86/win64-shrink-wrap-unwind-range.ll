; RUN: llc -mtriple=x86_64-unknown-windows-itanium -verify-machineinstrs < %s | FileCheck %s --check-prefixes=CHECK,X64
; RUN: llc -mtriple=x86_64-unknown-windows-itanium -filetype=obj < %s \
; RUN:   | llvm-readobj --unwind --symbols - | FileCheck %s --check-prefix=X64UNWIND
; RUN: %if aarch64-registered-target %{ llc -mtriple=aarch64-unknown-windows-itanium -aarch64-min-jump-table-entries=4 -verify-machineinstrs < %s | FileCheck %s --check-prefixes=CHECK,A64 %}
; RUN: %if aarch64-registered-target %{ llc -mtriple=aarch64-unknown-windows-itanium -aarch64-min-jump-table-entries=4 -filetype=obj < %s \
; RUN:   | llvm-readobj --unwind - | FileCheck %s --check-prefix=A64UNWIND %}

; Under Windows unwind info a shrink-wrapped function's unwind range is its
; personality is given, and the handler lies within the range. The linker may
; resize the code, so the range starts at a label, and its prologue's codes
; count from there. AArch64 unwind info records each epilogue's extent, so an
; epilogue that would fall out of the range into a return ends in its own copy
; of it. The object RUN lines have MC check each prologue's codes against its
; instructions.

declare void @may_throw()
declare void @use(ptr)
declare void @dtor(ptr)
declare void @g(i64)
declare void @h()
declare i32 @__gxx_personality_seh0(...)

; X64-LABEL: shrink_wrapped:
; X64-NEXT:  .Lfunc_begin0:
; X64-NEXT:  # %bb.0:
; X64-NEXT:    testq %rcx, %rcx
; X64-NEXT:    je [[EARLY:.LBB0_[0-9]+]]
; X64-NEXT:  [[BEGIN:.Lunwind_begin0]]:
; X64-NEXT:  .seh_proc shrink_wrapped
; X64-NEXT:    .seh_handler __gxx_personality_seh0, @unwind, @except
; X64-NEXT:  # %bb.1: # %work
; X64-NEXT:    pushq %rsi
; X64:         retq
; X64-NEXT:  [[CATCH:.LBB0_[0-9]+]]: # %catch
; X64:         .seh_endproc
; X64-NEXT:  [[EARLY]]: # %early
; X64-NEXT:    movl $-1, %eax
; X64-NEXT:    retq
; X64:         .uleb128 [[CATCH]]-[[BEGIN]]
; X64-NEXT:    .uleb128 [[SITE:.Ltmp[0-9]+]]-[[BEGIN]]
; X64:         .uleb128 [[SITE]]-[[BEGIN]] # >> Call Site 1 <<
; X64UNWIND:      StartAddress: shrink_wrapped +0x5 (0x0)
; X64UNWIND:        PrologSize: 5
; X64UNWIND:        UnwindCodes [
; X64UNWIND-NEXT:     0x05: ALLOC_SMALL size=32
; X64UNWIND-NEXT:     0x01: PUSH_NONVOL reg=RSI
; X64UNWIND-NEXT:   ]
; X64UNWIND-NEXT:   Handler: __gxx_personality_seh0
; A64UNWIND:      Function: {{.*}} (0x4)
; A64UNWIND:        Prologue [
; A64UNWIND-NEXT:     0xd600 ; stp x19, lr, [sp, #0]
; A64UNWIND-NEXT:     0x01 ; sub sp, #16
; A64UNWIND-NEXT:     0xe4 ; end
define i32 @shrink_wrapped(ptr %p) personality ptr @__gxx_personality_seh0 {
entry:
  %null = icmp eq ptr %p, null
  br i1 %null, label %early, label %work
early:
  ret i32 -1
work:
  %v = load i32, ptr %p
  invoke void @may_throw() to label %join unwind label %cs
cs:
  %s = catchswitch within none [label %catch] unwind to caller
catch:
  %cp = catchpad within %s [ptr null]
  catchret from %cp to label %join
join:
  %r = phi i32 [ %v, %work ], [ 0, %catch ]
  ret i32 %r
}

; A cleanup that the unwinder calls is a frame of its own, entered only by
; unwinding from a call within the range: it does not hold the prologue back,
; and its funclet follows the range, so the scope table counts to it from the
; range's start.
; CHECK-LABEL: with_cleanup:
; CHECK:         {{je|cbz x0,}} [[EXIT:.LBB1_[0-9]+]]
; CHECK-NEXT:  [[BEGIN1:.Lunwind_begin1]]:
; CHECK-NEXT:  .seh_proc with_cleanup
; CHECK:         .seh_endepilogue
; CHECK-NEXT:    ret
; CHECK:         .uleb128 [[CLEANUP:.LBB1_[0-9]+]]-[[BEGIN1]]
; CHECK-NEXT:    .uleb128 .Ltmp{{[0-9]+}}-[[BEGIN1]]
; CHECK:         .seh_endproc
; CHECK-NEXT:  [[EXIT]]:{{.*}}
; CHECK-NEXT:    {{retq|ret}}{{$}}
; CHECK:       "?dtor${{[0-9]+}}@?0?with_cleanup@4HA":
; CHECK-NEXT:  [[CLEANUP]]: {{#|//}} %cleanup
define void @with_cleanup(ptr %p) personality ptr @__gxx_personality_seh0 {
entry:
  %s = alloca i32
  %null = icmp eq ptr %p, null
  br i1 %null, label %early, label %work
early:
  ret void
work:
  invoke void @use(ptr %s) to label %done unwind label %cleanup
done:
  call void @dtor(ptr %s)
  ret void
cleanup:
  %cp = cleanuppad within none []
  call void @dtor(ptr %s) [ "funclet"(token %cp) ]
  cleanupret from %cp unwind to caller
}

; A64-LABEL: early:
; A64-NEXT:  // %bb.0:
; A64-NEXT:    cbz x0, [[OUT:.LBB2_[0-9]+]]
; A64-NEXT:  .Lunwind_begin2:
; A64-NEXT:  .seh_proc early
; A64-NEXT:  // %bb.1: // %work
; A64-NEXT:    sub sp, sp, #16
; A64:         .seh_endepilogue
; A64-NEXT:    ret
; A64-NEXT:    .seh_endfunclet
; A64-NEXT:    .seh_endproc
; A64-NEXT:  [[OUT]]: // %out
; A64-NEXT:    ret
; A64UNWIND:      Function:
; A64UNWIND:      Function:
; A64UNWIND:        Prologue [
; A64UNWIND-NEXT:     stp x19, lr, [sp]
; A64UNWIND-NEXT:     sub sp, sp, #16
; A64UNWIND-NEXT:     end
define void @early(i64 %x) {
entry:
  %c = icmp eq i64 %x, 0
  br i1 %c, label %out, label %work
work:
  call void @g(i64 %x)
  call void @h()
  call void @g(i64 %x)
  br label %out
out:
  ret void
}

; So is the version 3 marker of a function that needs it in a module that
; does not.
; X64-LABEL: shrink_wrapped_v3:
; X64:         je [[EARLY:.LBB4_[0-9]+]]
; X64-NEXT:  .Lunwind_begin4:
; X64-NEXT:  .seh_proc shrink_wrapped_v3
; X64-NEXT:  # %bb.{{[0-9]+}}: # %work
; X64-NEXT:    .seh_unwindversion 3
; X64:         .seh_endproc
; X64-NEXT:  [[EARLY]]: # %early
define i32 @shrink_wrapped_v3(ptr %p) "target-features"="+egpr" {
entry:
  %null = icmp eq ptr %p, null
  br i1 %null, label %early, label %work
early:
  ret i32 -1
work:
  call void @use(ptr %p)
  ret i32 0
}

; A save point that every path from the entry reaches spares no path the
; prologue, and its block would merge into the entry block behind the copy,
; leaving an instruction that is no part of the prologue in the unwind range:
; the prologue stays at the entry.
; A64-LABEL: straight:
; A64-NEXT:  .seh_proc straight
; A64-NEXT:  // %bb.0:
; A64-NEXT:    str x30, [sp, #-16]!
; A64-NEXT:    .seh_save_reg_x x30, 16
; A64-NEXT:    .seh_endprologue
; A64-NEXT:    mov w8, w0
; A64UNWIND:      Function: straight (
; A64UNWIND:        Prologue [
; A64UNWIND-NEXT:     0xd561 ; str x30, [sp, #-16]!
; A64UNWIND-NEXT:     0xe4 ; end
declare void @t0()
declare void @t1()
declare void @t2()
declare void @t3()
define void @straight(i32 %x) {
entry:
  switch i32 %x, label %def [
    i32 0, label %l0
    i32 1, label %l1
    i32 2, label %l2
    i32 3, label %l3
  ]
l0:
  call void @t0()
  ret void
l1:
  call void @t1()
  ret void
l2:
  call void @t2()
  ret void
l3:
  call void @t3()
  ret void
def:
  unreachable
}

; Code that tail duplication leaves after an epilogue is leaf code, which the
; unwinder would take for the body's: on x86-64 it reads an epilogue only where
; a return or a jump follows the stack release and the pops, and AArch64 unwind
; info ends an epilogue at its last instruction. Ahead of a return the code
; goes ahead of the epilogue; ahead of a branch the epilogue jumps to it, out
; of the range.

@flag = dso_local global i8 0

; X64-LABEL: leaf_return:
; X64:         callq h
; X64-NEXT:    movl $7, %eax
; X64-NEXT:    .seh_startepilogue
; X64-NEXT:    addq $40, %rsp
; X64-NEXT:    .seh_endepilogue
; X64-NEXT:    retq
; A64-LABEL: leaf_return:
; A64:         bl h
; A64-NEXT:    mov w0, #7
; A64-NEXT:    .seh_startepilogue
; A64-NEXT:    ldr x30, [sp], #16
; A64-NEXT:    .seh_save_reg_x x30, 16
; A64-NEXT:    .seh_endepilogue
; A64-NEXT:    ret
define i32 @leaf_return() {
entry:
  %f = load i8, ptr @flag
  %c = icmp eq i8 %f, 0
  br i1 %c, label %slow, label %end
slow:
  call void @may_throw()
  call void @h()
  br label %end
end:
  ret i32 7
}

; X64-LABEL: leaf_branch:
; X64:         .seh_startepilogue
; X64-NEXT:    addq $40, %rsp
; X64-NEXT:    .seh_endepilogue
; X64-NEXT:    jmp [[LEAF:.LBB[0-9_]+]]
; X64-NEXT:    .seh_endproc
; X64-NEXT:  [[LEAF]]:
; X64-NEXT:    cmpb $1, flag(%rip)
define void @leaf_branch() {
entry:
  %f = load i8, ptr @flag
  %c = icmp eq i8 %f, 0
  br i1 %c, label %slow, label %join
slow:
  call void @may_throw()
  call void @h()
  br label %join
join:
  %g = load i8, ptr @flag
  %e = icmp eq i8 %g, 1
  br i1 %e, label %a, label %b
a:
  store i8 1, ptr @flag
  ret void
b:
  store i8 2, ptr @flag
  store i8 3, ptr @flag
  ret void
}

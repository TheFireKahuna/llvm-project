; RUN: llc -mtriple=x86_64-unknown-windows-itanium -verify-machineinstrs < %s | FileCheck %s --check-prefixes=CHECK,X64
; RUN: llc -mtriple=x86_64-unknown-windows-itanium -filetype=obj < %s \
; RUN:   | llvm-readobj --unwind --symbols - | FileCheck %s --check-prefix=X64UNWIND
; RUN: %if aarch64-registered-target %{ llc -mtriple=aarch64-unknown-windows-itanium -aarch64-min-jump-table-entries=4 -verify-machineinstrs < %s | FileCheck %s --check-prefixes=CHECK,A64 %}
; RUN: %if aarch64-registered-target %{ llc -mtriple=aarch64-unknown-windows-itanium -aarch64-min-jump-table-entries=4 -filetype=obj < %s \
; RUN:   | llvm-readobj --unwind - | FileCheck %s --check-prefix=A64UNWIND %}

; Under Windows unwind info a shrink-wrapped function's unwind range is its
; frame, from a label at the prologue to after the epilogue, from which the
; exception tables count; code outside it is leaf code with no unwind info.
; A version 2 x86-64 epilogue may fall out of the range into the leaf code
; after it; an AArch64 one that would fall into a return ends in its own copy.
; The object RUN lines have MC check each prologue's codes against it.

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
; X64-NEXT:    .seh_unwindversion 2
; X64-NEXT:    pushq %rsi
; X64:         retq
; X64-NEXT:    .seh_handlerdata
; X64:         .uleb128 [[CATCH:.LBB0_[0-9]+]]-shrink_wrapped.cold
; X64-NEXT:    .uleb128 [[SITE:.Ltmp[0-9]+]]-[[BEGIN]]
; X64:         .uleb128 [[SITE]]-[[BEGIN]] # >> Call Site 1 <<
; X64:         .seh_endproc
; X64-NEXT:  [[EARLY]]: # %early
; X64-NEXT:    movl $-1, %eax
; X64-NEXT:    retq
; X64:         .section .text$split,"xr"
; X64-NEXT:  .seh_proc shrink_wrapped
; X64-NEXT:    .seh_unwindversion 2
; X64-NEXT:    .seh_pushreg %rsi
; X64-NEXT:    .seh_stackalloc 32
; X64-NEXT:    .seh_endprologue
; X64-NEXT:  shrink_wrapped.cold:
; X64-NEXT:    nop
; X64-NEXT:  [[CATCH]]: # %catch
; X64UNWIND:      StartAddress: shrink_wrapped +0x5 (0x0)
; X64UNWIND:        PrologSize: 5
; X64UNWIND:        UnwindCodes [
; X64UNWIND-NEXT:     EPILOG atend=no, length=0x2
; X64UNWIND-NEXT:     EPILOG offset=0x2
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
; and its funclet lies in the cold section, from whose start the scope table
; counts to it.
; CHECK-LABEL: with_cleanup:
; CHECK:         {{je|cbz x0,}} [[EXIT:.LBB1_[0-9]+]]
; CHECK-NEXT:  [[BEGIN1:.Lunwind_begin1]]:
; CHECK-NEXT:  .seh_proc with_cleanup
; X64:           callq dtor
; X64-NEXT:      addq $40, %rsp
; X64-NEXT:      .seh_handlerdata
; A64:           .seh_endepilogue
; A64-NEXT:      ret
; CHECK:         .uleb128 with_cleanup.cold-with_cleanup.cold
; CHECK-NEXT:    .uleb128 .Ltmp{{[0-9]+}}-[[BEGIN1]]
; CHECK:         .seh_endproc
; CHECK-NEXT:  [[EXIT]]:{{.*}}
; CHECK-NEXT:    {{retq|ret}}{{$}}
; CHECK:       "?dtor${{[0-9]+}}@?0?with_cleanup@4HA":
; CHECK-NEXT:  with_cleanup.cold: {{#|//}} %cleanup
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
; unwinder would take for the body's: an x86-64 epilogue's stated size takes
; in one instruction after the pops, and AArch64 unwind info ends an epilogue
; at its last instruction. Ahead of a return the code goes ahead of the
; epilogue; ahead of a branch it goes out of the range, into which the
; epilogue falls. The range ends with the last pop, and with no pop there is
; no epilogue to name.

@flag = dso_local global i8 0

; X64-LABEL: leaf_return:
; X64:         callq h
; X64-NEXT:    movl $7, %eax
; X64-NEXT:    .seh_startepilogue
; X64-NEXT:    addq $40, %rsp
; X64-NEXT:    .seh_unwindv2start
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
; X64:         callq h
; X64-NEXT:    addq $40, %rsp
; X64-NEXT:    .seh_endproc
; X64-NEXT:  # %bb.{{[0-9]+}}:
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

; X64-LABEL: leaf_fall:
; X64:         callq use
; X64-NEXT:    .seh_startepilogue
; X64-NEXT:    addq $32, %rsp
; X64-NEXT:    .seh_unwindv2start
; X64-NEXT:    popq %rsi
; X64-NEXT:    .seh_endepilogue
; X64-NEXT:    .seh_handlerdata
; X64:         .seh_endproc
; X64-NEXT:  # %bb.{{[0-9]+}}:
; X64-NEXT:    cmpb $1, flag(%rip)
define void @leaf_fall(ptr %p) personality ptr @__gxx_personality_seh0 {
entry:
  %f = load i8, ptr @flag
  %c = icmp eq i8 %f, 0
  br i1 %c, label %slow, label %join
slow:
  invoke void @may_throw() to label %cont unwind label %cleanup
cont:
  call void @use(ptr %p)
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
cleanup:
  %cp = cleanuppad within none []
  call void @h() [ "funclet"(token %cp) ]
  cleanupret from %cp unwind to caller
}

; A jump table's dispatch behind an epilogue leaves the range with its debug
; info, which labels the branch.

@word = dso_local global i64 0

; X64-LABEL: leaf_dispatch:
; X64:         popq %rsi
; X64-NEXT:    .seh_endepilogue
; X64-NEXT:    .seh_endproc
; X64-NEXT:  # %bb.{{[0-9]+}}:
; X64-NEXT:  .Ltmp{{[0-9]+}}:
; X64-NEXT:    jmpq *%r11
define i64 @leaf_dispatch(ptr %p, ptr %q, i1 %b, ptr %r) {
entry:
  %v = load i64, ptr @word
  %w = or i64 %v, 1
  store i64 %w, ptr @word
  %c = load i8, ptr %r
  switch i8 %c, label %def [
    i8 36, label %two
    i8 90, label %zero
    i8 89, label %zero1
    i8 86, label %zero2
    i8 85, label %zero3
    i8 84, label %zero4
  ]
zero:
  ret i64 0
zero1:
  ret i64 0
zero2:
  ret i64 0
zero3:
  ret i64 0
zero4:
  ret i64 0
two:
  br i1 %b, label %x, label %y
x:
  store ptr %r, ptr %q
  ret i64 %v
y:
  store i64 %w, ptr @word
  ret i64 0
def:
  store i8 0, ptr %p
  ret i64 0
}

; The cold part of shrink_wrapped has unwind info of its own, the last in the
; object: the prologue's unwind codes with no prologue, which on AArch64 follow
; end_c.
; X64UNWIND:      StartAddress: shrink_wrapped.cold
; X64UNWIND:        Version: 2
; X64UNWIND:        PrologSize: 0
; X64UNWIND:        UnwindCodes [
; X64UNWIND-NEXT:     0x00: ALLOC_SMALL size=32
; X64UNWIND-NEXT:     0x00: PUSH_NONVOL reg=RSI
; X64UNWIND-NEXT:   ]
; A64UNWIND:          0xe5 ; end_c
; A64UNWIND-NEXT:     0xd600 ; stp x19, lr, [sp, #0]
; A64UNWIND-NEXT:     0x01 ; sub sp, #16
; A64UNWIND-NEXT:     0xe4 ; end

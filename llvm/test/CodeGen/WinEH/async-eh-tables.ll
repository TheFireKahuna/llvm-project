; RUN: llc -mtriple=x86_64-unknown-windows-itanium -verify-machineinstrs < %s | FileCheck %s
; RUN: llc -mtriple=x86_64-unknown-windows-itanium -filetype=obj -o %t.obj < %s
; RUN: llvm-objdump -d --no-show-raw-insn %t.obj | FileCheck %s --check-prefix=DIS
; RUN: %if aarch64-registered-target %{ llc -mtriple=aarch64-unknown-windows-itanium -verify-machineinstrs < %s | FileCheck %s --check-prefix=A64 --implicit-check-not=eh_sites %}
; RUN: %if aarch64-registered-target %{ llc -mtriple=aarch64-unknown-windows-itanium -filetype=obj -o /dev/null < %s %}
; RUN: %if aarch64-registered-target %{ llc -mtriple=aarch64-unknown-windows-itanium -O0 -verify-machineinstrs < %s | FileCheck %s --check-prefix=A64O0 %}

; A function that names __gxx_personality_seha0 takes the exceptions that any
; of its instructions raises: its exception table covers every instruction of
; an EH scope, to the byte. AArch64's unwinder says whether the address it
; gives is a return address or that of the instruction that raised, so there a
; range is written where its instructions are, and no nop follows a call that
; ends one.

@g = external global i32
declare void @may_throw()
declare void @ctor(ptr)
declare void @dtor(ptr) nounwind
declare i32 @__gxx_personality_seha0(...)
declare void @llvm.seh.scope.begin()
declare void @llvm.seh.scope.end()
declare void @llvm.seh.try.begin()
declare void @llvm.seh.try.end()

; The range of the object's scope starts after its constructor's call and ends
; ahead of its destructor's. A table is searched with the address before the
; frame's, that of an instruction that raised or a return address, so every
; range is written a byte early: the store in the scope is found in its range,
; and the instruction after it is not. The constructor's call ends the range
; ahead of the scope, and a nop keeps its return address in that one. Handlers
; and funclets are where they are.
define void @scope(ptr %p) personality ptr @__gxx_personality_seha0 {
; CHECK-LABEL: scope:
; CHECK-NEXT:  .Lfunc_begin0:
; CHECK-NOT:     nop
; CHECK:         callq ctor
; CHECK-NEXT:    nop
; CHECK-NEXT:  [[BEGIN:.Ltmp[0-9]+]]:
; CHECK-NEXT:    movl $1, (%rsi)
; CHECK-NEXT:  [[END:.Ltmp[0-9]+]]:
; CHECK-NEXT:  # %bb.{{[0-9]+}}: # %out
; CHECK-NEXT:    leaq {{.*}}, %rcx
; CHECK-NEXT:    callq dtor
; CHECK-NEXT:    movl $2, (%rsi)
;
; The scope is the funclet's, its site is the scope's range, and the code
; outside the scope has entries with no landing pad.
; CHECK:       .Leh_sites0 = .Lfunc_begin0+1
; CHECK-NEXT:    .byte 2 # Scope table version
; CHECK:         .uleb128 [[FUNCLET:.LBB0_[0-9]+]]-.Lfunc_begin0
; CHECK-NEXT:    .uleb128 [[BEGIN]]-.Leh_sites0
; CHECK-NEXT:    .byte 1
; CHECK-NEXT:  .Lscopes_end0:
; CHECK:         .uleb128 .Leh_sites0-.Leh_sites0 # >> Call Site 1 <<
; CHECK-NEXT:    .uleb128 [[BEGIN]]-.Leh_sites0
; CHECK-NEXT:    .byte 0 # has no landing pad
; CHECK-NEXT:    .byte 0 # On action: cleanup
; CHECK-NEXT:    .uleb128 [[BEGIN]]-.Leh_sites0 # >> Call Site 2 <<
; CHECK-NEXT:    .uleb128 [[END]]-[[BEGIN]]
; CHECK-NEXT:    .byte 0 # has no landing pad
; CHECK-NEXT:    .byte 0 # On action: cleanup
; CHECK-NEXT:    .uleb128 [[END]]-.Leh_sites0 # >> Call Site 3 <<
; CHECK-NEXT:    .uleb128 [[FUNCLET]]-[[END]]
; CHECK-NEXT:    .byte 0 # has no landing pad
; CHECK-NEXT:    .byte 0 # On action: cleanup
; CHECK-NEXT:  .Lcst_end0:
;
; The store is at 0x19 and the funclet at 0x35. The scope table names the
; funclet and the scope's site at 0x18, and the call sites are 0 to 0x18, six
; bytes from 0x18 and the rest from 0x1e. The linker may resize the code, so
; the tables' spans name labels.
; DIS-LABEL: <scope>:
; DIS:            18: nop
; DIS-EMPTY:
; DIS-NEXT:       <$L{{[0-9a-f]+}}>:
; DIS-NEXT:       19: movl $0x1, (%rsi)
; DIS-EMPTY:
; DIS-NEXT:       <$L{{[0-9a-f]+}}>:
; DIS-NEXT:       1f: leaq
; DIS:            35: leaq 0x2c(%rdx), %rcx
; DIS-NEXT:       39: jmp
; A64-LABEL: scope:
; A64:         bl ctor
; A64-NEXT:  // %bb.{{[0-9]+}}:
; A64-NOT:     nop
; A64-NEXT:  [[BEGIN:.Ltmp[0-9]+]]:
; A64-NEXT:    mov w8, #1
; A64-NEXT:    str w8, [x19]
; A64-NEXT:  [[END:.Ltmp[0-9]+]]:
; A64-NEXT:    add x0, x29, #20
; A64-NEXT:    bl dtor
; A64:         .uleb128 .LBB0_{{[0-9]+}}-.Lfunc_begin0
; A64-NEXT:    .uleb128 [[BEGIN]]-.Lfunc_begin0
; A64:         .uleb128 [[BEGIN]]-.Lfunc_begin0 // >> Call Site 2 <<
; A64-NEXT:    .uleb128 [[END]]-[[BEGIN]]
;
; The markers are invokes of intrinsics, which GlobalISel leaves to
; SelectionDAG.
; A64O0-LABEL: scope:
; A64O0:          [[BEGIN:.Ltmp[0-9]+]]:
; A64O0:            str w8, [x9]
; A64O0-NEXT:     [[END:.Ltmp[0-9]+]]:
; A64O0:            .uleb128 [[BEGIN]]-.Lfunc_begin0 // >> Call Site 2 <<
; A64O0-NEXT:       .uleb128 [[END]]-[[BEGIN]]
entry:
  %a = alloca i32
  store i32 0, ptr %p
  call void @ctor(ptr %a)
  invoke void @llvm.seh.scope.begin() to label %in unwind label %cleanup
in:
  store i32 1, ptr %p
  invoke void @llvm.seh.scope.end() to label %out unwind label %cleanup
out:
  call void @dtor(ptr %a)
  store i32 2, ptr %p
  ret void
cleanup:
  %pad = cleanuppad within none []
  call void @dtor(ptr %a) [ "funclet"(token %pad) ]
  cleanupret from %pad unwind to caller
}

; The instructions of a try block and its call make one range of the try
; block's scope. The handler is outside it, and a fault may land in it between
; any two instructions, where only memory holds a value: the pointer is in its
; slot from before the try block.
define void @try(ptr %p) personality ptr @__gxx_personality_seha0 {
; CHECK-LABEL: try:
; CHECK:         movq %rcx, [[SLOT:[0-9]+]](%rsp)
; CHECK-NEXT:  [[BEGIN:.Ltmp[0-9]+]]:
; CHECK-NEXT:    movq [[SLOT]](%rsp), %rax
; CHECK-NEXT:    movl $1, (%rax)
; CHECK-NEXT:  .Ltmp{{[0-9]+}}:
; CHECK-NEXT:  .Ltmp{{[0-9]+}}:
; CHECK-NEXT:    callq may_throw
; CHECK-NEXT:    nop
; CHECK-NEXT:  [[END:.Ltmp[0-9]+]]:
; CHECK:       [[PAD:.LBB1_[0-9]+]]: # %catch
; CHECK-NEXT:  $ehgcr_1_{{[0-9]+}}:
; CHECK-NEXT:    movq [[SLOT]](%rsp), %rax
; CHECK-NEXT:    movl $2, (%rax)
; CHECK:         .uleb128 [[BEGIN]]-.Leh_sites1 # >> Call Site 2 <<
; CHECK-NEXT:    .uleb128 [[END]]-[[BEGIN]]
; CHECK-NEXT:    .uleb128 [[PAD]]-.Lfunc_begin1
; CHECK-NEXT:    .byte 1 # On action: 1
; CHECK-NEXT:    .uleb128 [[END]]-.Leh_sites1 # >> Call Site 3 <<
; CHECK-NEXT:    .uleb128 .Lfunc_end1-[[END]]
; CHECK-NEXT:    .byte 0 # has no landing pad
entry:
  invoke void @llvm.seh.try.begin() to label %body unwind label %cs
body:
  store i32 1, ptr %p
  invoke void @may_throw() to label %cont unwind label %cs
cont:
  invoke void @llvm.seh.try.end() to label %out unwind label %cs
cs:
  %s = catchswitch within none [label %catch] unwind to caller
catch:
  %cp = catchpad within %s [ptr null]
  catchret from %cp to label %handler
handler:
  store i32 2, ptr %p
  br label %out
out:
  ret void
}

; A call has a nop behind it only where it is the last instruction of a range:
; the constructor's, and the second call in the scope. The return address of
; the first is in a range of the same cleanup.
define void @calls(ptr %p) personality ptr @__gxx_personality_seha0 {
; CHECK-LABEL: calls:
; CHECK:         callq ctor
; CHECK-NEXT:    nop
; CHECK-NEXT:  [[BEGIN:.Ltmp[0-9]+]]:
; CHECK-NEXT:    callq may_throw
; CHECK-NEXT:  .Ltmp{{[0-9]+}}:
; CHECK-NEXT:  # %bb.{{[0-9]+}}:
; CHECK-NEXT:  .Ltmp{{[0-9]+}}:
; CHECK-NEXT:    movl $1, (%rsi)
; CHECK-NEXT:  .Ltmp{{[0-9]+}}:
; CHECK-NEXT:  .Ltmp{{[0-9]+}}:
; CHECK-NEXT:    callq may_throw
; CHECK-NEXT:    nop
; CHECK-NEXT:  [[END:.Ltmp[0-9]+]]:
; CHECK-NEXT:  # %bb.{{[0-9]+}}: # %out
; CHECK-NEXT:    leaq {{.*}}, %rcx
; CHECK:         .uleb128 [[BEGIN]]-.Leh_sites2 # >> Call Site 2 <<
; CHECK-NEXT:    .uleb128 [[END]]-[[BEGIN]]
entry:
  %a = alloca i32
  call void @ctor(ptr %a)
  invoke void @llvm.seh.scope.begin() to label %in unwind label %cleanup
in:
  invoke void @may_throw() to label %cont unwind label %cleanup
cont:
  store i32 1, ptr %p
  invoke void @may_throw() to label %last unwind label %cleanup
last:
  invoke void @llvm.seh.scope.end() to label %out unwind label %cleanup
out:
  call void @dtor(ptr %a)
  ret void
cleanup:
  %pad = cleanuppad within none []
  call void @dtor(ptr %a) [ "funclet"(token %pad) ]
  cleanupret from %pad unwind to caller
}

; A range that begins the function cannot begin a byte early, so a nop goes
; ahead of it, and only there: a function that has an instruction ahead of its
; first range, as those above, has none. The function needs no frame and its
; funclet has one.
define void @first(ptr %p) personality ptr @__gxx_personality_seha0 {
; CHECK-LABEL: first:
; CHECK-NEXT:  .Lfunc_begin3:
; CHECK-NEXT:  .seh_proc first
; CHECK-NEXT:    .seh_handler __gxx_personality_seha0, @unwind, @except
; CHECK-NEXT:    nop
; CHECK-NEXT:  # %bb.0: # %entry
; CHECK-NEXT:    .seh_endprologue
; CHECK-NEXT:  [[BEGIN:.Ltmp[0-9]+]]:
; CHECK-NEXT:    movl $1, (%rcx)
; CHECK-NEXT:  [[END:.Ltmp[0-9]+]]:
; CHECK:         .uleb128 [[FUNCLET:.LBB3_[0-9]+]]-.Lfunc_begin3
; CHECK-NEXT:    .uleb128 [[BEGIN]]-.Leh_sites3
; CHECK:         .uleb128 [[BEGIN]]-.Leh_sites3 # >> Call Site 1 <<
; CHECK-NEXT:    .uleb128 [[END]]-[[BEGIN]]
; DIS-LABEL: <first>:
; DIS-NEXT:       nop
; DIS-EMPTY:
; DIS-NEXT:       <$L{{[0-9a-f]+}}>:
; DIS-NEXT:       movl $0x1, (%rcx)
entry:
  invoke void @llvm.seh.scope.begin() to label %in unwind label %cleanup
in:
  store i32 1, ptr %p
  invoke void @llvm.seh.scope.end() to label %out unwind label %cleanup
out:
  ret void
cleanup:
  %pad = cleanuppad within none []
  store volatile i32 2, ptr @g
  cleanupret from %pad unwind to caller
}

; Code ahead of a prologue is leaf code, which an asynchronous exception can
; reach, so a function with these tables is not shrink-wrapped.
; CHECK-LABEL: early_exit:
; CHECK:         .seh_endprologue
; CHECK-NEXT:    testq %rcx, %rcx
; A64-LABEL:   early_exit:
; A64:           .seh_endprologue
; A64-NEXT:      cbz x0
define void @early_exit(ptr %p) personality ptr @__gxx_personality_seha0 {
entry:
  %a = alloca i32
  %null = icmp eq ptr %p, null
  br i1 %null, label %exit, label %work
work:
  call void @ctor(ptr %a)
  invoke void @llvm.seh.scope.begin() to label %in unwind label %cleanup
in:
  store i32 1, ptr %p
  invoke void @llvm.seh.scope.end() to label %out unwind label %cleanup
out:
  call void @dtor(ptr %a)
  br label %exit
exit:
  ret void
cleanup:
  %pad = cleanuppad within none []
  call void @dtor(ptr %a) [ "funclet"(token %pad) ]
  cleanupret from %pad unwind to caller
}

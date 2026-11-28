; RUN: llc -mtriple=x86_64-unknown-windows-itanium -verify-machineinstrs -emit-call-site-info < %s | FileCheck %s --check-prefixes=CHECK,EHCONT
; RUN: llc -mtriple=x86_64-unknown-windows-itanium -O0 -verify-machineinstrs < %s | FileCheck %s --check-prefix=O0
; RUN: llc -mtriple=x86_64-unknown-windows-itanium -frame-pointer=all -verify-machineinstrs < %s | FileCheck %s --check-prefix=FP
; RUN: llc -mtriple=x86_64-unknown-windows-itanium -stop-after=finalize-isel < %s | FileCheck %s --check-prefix=MIR
; RUN: %if aarch64-registered-target %{ llc -mtriple=aarch64-unknown-windows-itanium -verify-machineinstrs < %s | FileCheck %s --check-prefixes=A64,EHCONT %}
; RUN: %if aarch64-registered-target %{ llc -mtriple=aarch64-unknown-windows-itanium -filetype=obj -o /dev/null < %s %}
; RUN: %if aarch64-registered-target %{ llc -mtriple=aarch64-unknown-windows-itanium -O0 -verify-machineinstrs < %s | FileCheck %s --check-prefix=A64O0 %}

; Under an Itanium personality on Windows Itanium a cleanuppad is a funclet
; that only the unwinder calls, and a catchpad is landed: it is code of the
; and of each call site's innermost scope; then the LSDA.

declare void @may_throw()
declare void @free(ptr) nounwind
declare void @dtor(ptr) nounwind
declare void @use9(ptr, i64, i64, i64, i64, i64, i64, i64, i64) nounwind
declare void @__cxa_call_unexpected(ptr) noreturn nounwind
declare i32 @__gxx_personality_seh0(...)
declare ptr @llvm.eh.exceptionpointer.p0(token)
@_ZTIi = external constant ptr
@c1 = external dso_local constant i64
@c2 = external dso_local constant i64
@c3 = external dso_local constant i64
@g = external dso_local global i64

; A cleanup alone: the site names the cleanup's scope, and the LSDA's site
; has no pad. No nop keeps the call's return address ahead
; of the label. The function keeps no frame pointer for the funclet, which
; reaches the frame from the establisher frame in RDX and is not aligned. Its
; call is a tail call, so it allocates nothing and is a leaf with no unwind
; record: what unwinds out of it reaches the unwinder that called it.
; CHECK-LABEL: cleanup:
; CHECK:         .seh_handler __gxx_personality_seh0, @unwind, @except
; CHECK-NEXT:  # %bb.0:
; CHECK-NEXT:    subq $40, %rsp
; CHECK-NEXT:    .seh_stackalloc 40
; CHECK-NEXT:    .seh_endprologue
; CHECK-NEXT:  [[BEGIN:.Ltmp[0-9]+]]:
; CHECK-NEXT:    callq may_throw
; CHECK-NEXT:  [[END:.Ltmp[0-9]+]]:
; CHECK:         .uleb128 [[FUNCLET:.LBB0_[0-9]+]]-[[FUNC:.Lfunc_begin0]]
; CHECK-NEXT:    .uleb128 [[BEGIN]]-[[FUNC]]
; CHECK-NEXT:    .byte 1
; CHECK-NEXT:  .Lscopes_end0:
; CHECK:         .uleb128 [[BEGIN]]-[[FUNC]]
; CHECK-NEXT:    .uleb128 [[END]]-[[BEGIN]]
; CHECK-NEXT:    .byte 0 # has no landing pad
; CHECK-NEXT:    .byte 0
; CHECK-NEXT:  .Lcst_end0:
; CHECK:         .seh_endproc
; CHECK:       "?dtor$2@?0?cleanup@4HA":
; CHECK-NEXT:  [[FUNCLET]]:
; CHECK-NEXT:    leaq 36(%rdx), %rcx
; CHECK-NEXT:    jmp dtor # TAILCALL
; CHECK-NEXT:  .Lfunc_end0:
; CHECK-NOT:     .seh_
; A64:       "?dtor$2@?0?cleanup@4HA":
; A64-NEXT:  .LBB0_2:
; A64-NEXT:    sub x0, x29, #4
; A64-NEXT:    b dtor
; A64-NEXT:  .Lfunc_end0:
; A64-NOT:     .seh_
;
; Unoptimized code keeps a frame pointer, which its funclets are entered with.
; O0-LABEL: cleanup:
; O0:         movq %rsp, %rbp
; O0:       "?dtor$2@?0?cleanup@4HA":
; O0-NEXT:  .LBB0_2:
; O0-NEXT:    leaq -4(%rbp), %rcx
; O0-NEXT:    jmp dtor # TAILCALL
; A64O0:      add x29, sp, #16
; A64O0:      .LBB0_2:
; A64O0-NEXT:   sub x0, x29, #4
; A64O0-NEXT:   b dtor
define void @cleanup() personality ptr @__gxx_personality_seh0 {
entry:
  %a = alloca i32
  invoke void @may_throw() to label %cont unwind label %ehcleanup
cont:
  call void @dtor(ptr %a)
  ret void
ehcleanup:
  %pad = cleanuppad within none []
  call void @dtor(ptr %a) [ "funclet"(token %pad) ]
  cleanupret from %pad unwind to caller
}

; and takes the exception object's address in RAX.
; CHECK-LABEL: nested:
; CHECK:       [[BEGIN1:.Ltmp[0-9]+]]:
; CHECK-NEXT:    callq may_throw
; CHECK-NEXT:  [[END1:.Ltmp[0-9]+]]:
; CHECK:       [[BEGIN2:.Ltmp[0-9]+]]:
; CHECK-NEXT:    callq may_throw
; CHECK:       [[INT:.LBB1_[0-9]+]]: # %inner.catch
; CHECK-NEXT:  $ehgcr_1_{{[0-9]+}}:
; CHECK-NEXT:    movl $1, %eax
; CHECK:       [[ALL:.LBB1_[0-9]+]]: # %outer.catch
; CHECK-NEXT:  $ehgcr_1_{{[0-9]+}}:
; CHECK-NEXT:    movq %rax, %rcx
; CHECK-NEXT:    callq dtor
; A64-LABEL: nested:
; A64:         // %outer.catch
; A64-NEXT:  $ehgcr_1_{{[0-9]+}}:
; A64-NEXT:    bl dtor
; CHECK:         .seh_handlerdata
; CHECK-NEXT:    .p2align 2
; CHECK-NEXT:    .byte 2 # Scope table version
; CHECK-NEXT:    .uleb128 .Lscopes_end1-.Lscopes_begin1
; CHECK-NEXT:  .Lscopes_begin1:
; CHECK-NEXT:    .byte 4 # Scopes
; CHECK-NEXT:    .byte 0
; CHECK-NEXT:    .byte 1
; CHECK-NEXT:    .byte 1
; CHECK-NEXT:    .uleb128 [[ALL]]-.Lfunc_begin1
; CHECK-NEXT:    .byte 1
; CHECK-NEXT:    .byte 0
; CHECK-NEXT:    .uleb128 [[CLEANUPA:.LBB1_[0-9]+]]-.Lfunc_begin1
; CHECK-NEXT:    .byte 2
; CHECK-NEXT:    .byte 1
; CHECK-NEXT:    .byte 2
; CHECK-NEXT:    .uleb128 [[INT]]-.Lfunc_begin1
; CHECK-NEXT:    .byte 3
; CHECK-NEXT:    .byte 0
; CHECK-NEXT:    .uleb128 [[CLEANUPB:.LBB1_[0-9]+]]-.Lfunc_begin1
; CHECK-NEXT:    .uleb128 [[BEGIN1]]-.Lfunc_begin1
; CHECK-NEXT:    .byte 4
; CHECK-NEXT:    .uleb128 [[BEGIN2]]-.Lfunc_begin1
; CHECK-NEXT:    .byte 2
; CHECK-NEXT:  .Lscopes_end1:
; CHECK:       .Lcst_begin1:
; CHECK-NEXT:    .uleb128 [[BEGIN1]]-.Lfunc_begin1
; CHECK-NEXT:    .uleb128 [[END1]]-[[BEGIN1]]
; CHECK-NEXT:    .uleb128 [[INT]]-.Lfunc_begin1
; CHECK-NEXT:    .byte 3 # On action: 2
; CHECK-NEXT:    .uleb128 [[BEGIN2]]-.Lfunc_begin1
; CHECK-NEXT:    .uleb128
; CHECK-NEXT:    .uleb128 [[ALL]]-.Lfunc_begin1
; CHECK-NEXT:    .byte 1 # On action: 1
; CHECK-NEXT:  .Lcst_end1:
; CHECK-NEXT:    .byte 1 # >> Action Record 1 <<
; CHECK-NEXT:          # Catch TypeInfo 1
; CHECK-NEXT:    .byte 0
; CHECK-NEXT:    .byte 2 # >> Action Record 2 <<
; CHECK-NEXT:          # Catch TypeInfo 2
; CHECK-NEXT:    .byte 125 # Continue to action 1
; CHECK:       [[TI:.Ltmp[0-9]+]]: # TypeInfo 2
; CHECK-NEXT:    .long __imp__ZTIi-[[TI]]
; CHECK-NEXT:    .long 0 # TypeInfo 1
; CHECK:       [[CLEANUPB]]: # %cleanup.b
; CHECK:       [[CLEANUPA]]: # %cleanup.a
define i32 @nested() personality ptr @__gxx_personality_seh0 {
entry:
  %a = alloca i32
  %b = alloca i32
  invoke void @may_throw() to label %mid unwind label %cleanup.b
mid:
  invoke void @may_throw() to label %cont unwind label %cleanup.a
cont:
  ret i32 0
cleanup.b:
  %pb = cleanuppad within none []
  call void @dtor(ptr %b) [ "funclet"(token %pb) ]
  cleanupret from %pb unwind label %inner.cs
inner.cs:
  %ics = catchswitch within none [label %inner.catch] unwind label %cleanup.a
inner.catch:
  %icp = catchpad within %ics [ptr @_ZTIi]
  catchret from %icp to label %inner.body
inner.body:
  ret i32 1
cleanup.a:
  %pa = cleanuppad within none []
  call void @dtor(ptr %a) [ "funclet"(token %pa) ]
  cleanupret from %pa unwind label %outer.cs
outer.cs:
  %ocs = catchswitch within none [label %outer.catch] unwind to caller
outer.catch:
  %ocp = catchpad within %ocs [ptr null]
  %exn = call ptr @llvm.eh.exceptionpointer.p0(token %ocp)
  catchret from %ocp to label %outer.body
outer.body:
  call void @dtor(ptr %exn)
  ret i32 2
}

; An exception specification is a pad for the filter's selector, which calls
; __cxa_call_unexpected with the exception object's address.
; CHECK-LABEL: spec:
; CHECK:       [[PAD:.LBB2_[0-9]+]]: # %unexpected
; CHECK-NEXT:  $ehgcr_2_{{[0-9]+}}:
; CHECK-NEXT:    movq %rax, %rcx
; CHECK-NEXT:    callq __cxa_call_unexpected
; CHECK:       .Lscopes_begin2:
; CHECK-NEXT:    .byte 2 # Scopes
; CHECK-NEXT:    .byte 0
; CHECK-NEXT:    .byte 1
; CHECK-NEXT:    .byte 127
; CHECK-NEXT:    .uleb128 [[PAD]]-.Lfunc_begin2
; CHECK:         .byte 127 # >> Action Record 1 <<
; CHECK-NEXT:          # Filter TypeInfo -1
; CHECK:       [[TI:.Ltmp[0-9]+]]: # TypeInfo 1
; CHECK-NEXT:    .long __imp__ZTIi-[[TI]]
; CHECK-NEXT:  .Lttbase{{[0-9]+}}:
; CHECK-NEXT:          # >> Filter TypeInfos <<
; CHECK-NEXT:    .byte 1
; CHECK-NEXT:    .byte 0
define void @spec() personality ptr @__gxx_personality_seh0 {
entry:
  %a = alloca i32
  invoke void @may_throw() to label %cont unwind label %ehcleanup
cont:
  ret void
ehcleanup:
  %pad = cleanuppad within none []
  call void @dtor(ptr %a) [ "funclet"(token %pad) ]
  cleanupret from %pad unwind label %spec.cs
spec.cs:
  %cs = catchswitch within none [label %unexpected] unwind to caller
unexpected:
  %cp = catchpad within %cs [[1 x ptr] [ptr @_ZTIi]]
  %exn = call ptr @llvm.eh.exceptionpointer.p0(token %cp)
  catchret from %cp to label %call
call:
  call void @__cxa_call_unexpected(ptr %exn)
  unreachable
}

; A function with a catch and no cleanup has no funclet. The unwinder lands in
; the pad with the function's callee-saved registers, so a value can stay in
; one across the call into the handler, and the function saves it.
; CHECK-LABEL: plain:
; CHECK:         pushq %rsi
; CHECK-NEXT:    .seh_pushreg %rsi
; CHECK:         movq %rcx, %rsi
; CHECK-NEXT:  .Ltmp{{[0-9]+}}:
; CHECK-NEXT:    callq may_throw
; CHECK:       # %catch
; CHECK-NEXT:  $ehgcr_3_{{[0-9]+}}:
; CHECK-NEXT:    incq %rsi
; CHECK-NOT:   .seh_proc
; CHECK:       .Lscopes_begin3:
; CHECK-NEXT:    .byte 1 # Scopes
define i64 @plain(i64 %x) personality ptr @__gxx_personality_seh0 {
entry:
  invoke void @may_throw() to label %cont unwind label %cs
cont:
  ret i64 %x
cs:
  %s = catchswitch within none [label %catch] unwind to caller
catch:
  %cp = catchpad within %s [ptr null]
  catchret from %cp to label %body
body:
  %y = add i64 %x, 1
  ret i64 %y
}

; A function with a frame pointer of its own enters its funclets with it, and
; they address the frame from it. The funclet's prologue allocates its calls'
; frame, where the function adjusts the stack around each of its own, so its
; unwind record holds at every call.
; CHECK-LABEL: frame_pointer:
; CHECK:         .seh_setframe %rbp, 0
; CHECK:       "?dtor$2@?0?frame_pointer@4HA":
; CHECK:       .LBB4_2:
; CHECK-NEXT:    subq $40, %rsp
; CHECK-NEXT:    .seh_stackalloc 40
; CHECK-NEXT:    .seh_endprologue
; CHECK-NEXT:    leaq -4(%rbp), %rcx
; CHECK-NEXT:    callq dtor
; CHECK-NEXT:    leaq -8(%rbp), %rcx
; CHECK-NEXT:    .seh_startepilogue
; CHECK-NEXT:    addq $40, %rsp
; CHECK-NEXT:    .seh_endepilogue
; CHECK-NEXT:    jmp dtor # TAILCALL
define void @frame_pointer(i64 %n) personality ptr @__gxx_personality_seh0 {
entry:
  %a = alloca i32
  %b = alloca i32
  %v = alloca i8, i64 %n
  call void @dtor(ptr %v)
  invoke void @may_throw() to label %cont unwind label %ehcleanup
cont:
  ret void
ehcleanup:
  %pad = cleanuppad within none []
  call void @dtor(ptr %b) [ "funclet"(token %pad) ]
  call void @dtor(ptr %a) [ "funclet"(token %pad) ]
  cleanupret from %pad unwind to caller
}

; A value of the function's that a funclet reads reaches it in the
; callee-saved register where the function keeps it; one that a load of a
; constant gives is loaded again.
; CHECK-LABEL: crossing:
; CHECK:         pushq %rsi
; CHECK-NOT:     pushq
; CHECK:         movq c1(%rip), %rsi
; CHECK-NEXT:    addq c2(%rip), %rsi
; CHECK-NEXT:    movq %rsi, (%rcx)
; CHECK-NEXT:    movq c3(%rip), %rax
; CHECK-NEXT:    movq %rax, (%rdx)
; CHECK:       .LBB5_2:
; CHECK-NEXT:    movq c3(%rip), %rax
; CHECK-NEXT:    movq %rax, 32(%rdx)
; CHECK-NEXT:    movq %rsi, 40(%rdx)
; CHECK-NEXT:    leaq 32(%rdx), %rcx
; CHECK-NEXT:    jmp dtor # TAILCALL
define void @crossing(ptr %x, ptr %y) personality ptr @__gxx_personality_seh0 {
entry:
  %a = alloca [2 x i64]
  %p1 = load i64, ptr @c1
  %p2 = load i64, ptr @c2
  %s = add i64 %p1, %p2
  store i64 %s, ptr %x
  %p3 = load i64, ptr @c3
  store i64 %p3, ptr %y
  invoke void @may_throw() to label %cont unwind label %ehcleanup
cont:
  ret void
ehcleanup:
  %pad = cleanuppad within none []
  %q1 = load i64, ptr @c1
  %q2 = load i64, ptr @c2
  %t = add i64 %q1, %q2
  %q3 = load i64, ptr @c3
  store i64 %q3, ptr %a
  %a1 = getelementptr i64, ptr %a, i64 1
  store i64 %t, ptr %a1
  call void @dtor(ptr %a) [ "funclet"(token %pad) ]
  cleanupret from %pad unwind to caller
}

; A funclet saves no callee-saved register: the unwinder's routine gives it the
; frame's and restores its own. Nor does the function save one for it. One with
; a frame has unwind codes and no handler: a search that leaves it ends at the
; unwinder's routine.
; CHECK-LABEL: saves:
; CHECK:       # %bb.0:
; CHECK-NEXT:    subq $56, %rsp
; CHECK:       "?dtor$2@?0?saves@4HA":
; CHECK-NEXT:  .seh_proc "?dtor$2@?0?saves@4HA"
; CHECK-NEXT:  .LBB6_2:
; CHECK-NEXT:    subq $40, %rsp
; CHECK-NEXT:    .seh_stackalloc 40
; CHECK-NEXT:    .seh_endprologue
; CHECK-NEXT:    movl $4, %edi
; CHECK-NEXT:    leaq 40(%rdx), %rsi
; CHECK:         callq dtor
; CHECK:         .seh_startepilogue
; CHECK-NEXT:    addq $40, %rsp
; CHECK-NEXT:    .seh_endepilogue
; CHECK-NEXT:    retq # CLEANUPRET
; CHECK-NEXT:  .Lfunc_end6:
; CHECK-NEXT:    .size saves, .Lfunc_end6-saves
; CHECK-NEXT:    .seh_endproc
define void @saves() personality ptr @__gxx_personality_seh0 {
entry:
  %a = alloca [4 x i32]
  invoke void @may_throw() to label %cont unwind label %ehcleanup
cont:
  ret void
ehcleanup:
  %pad = cleanuppad within none []
  br label %loop
loop:
  %i = phi i64 [ 0, %ehcleanup ], [ %n, %loop ]
  %e = getelementptr [4 x i32], ptr %a, i64 0, i64 %i
  call void @dtor(ptr %e) [ "funclet"(token %pad) ]
  %n = add i64 %i, 1
  %c = icmp eq i64 %n, 4
  br i1 %c, label %done, label %loop
done:
  cleanupret from %pad unwind to caller
}

; A funclet of a function with a realigned stack takes the base register from
; the establisher frame as the function's prologue takes it from the stack
; pointer.
; CHECK-LABEL: base_register:
; CHECK:         leaq 96(%rsp), %rbp
; CHECK-NEXT:    .seh_setframe %rbp, 96
; CHECK-NEXT:    .seh_endprologue
; CHECK-NEXT:    andq $-64, %rsp
; CHECK-NEXT:    movq %rsp, %rbx
; CHECK:       .LBB7_2:
; CHECK-NEXT:    pushq %rbp
; CHECK-NEXT:    .seh_pushreg %rbp
; CHECK-NEXT:    subq $32, %rsp
; CHECK-NEXT:    .seh_stackalloc 32
; CHECK-NEXT:    leaq 96(%rdx), %rbp
; CHECK-NEXT:    .seh_endprologue
; CHECK-NEXT:    andq $-64, %rdx
; CHECK-NEXT:    movq %rdx, %rbx
; CHECK-NEXT:    leaq 64(%rbx), %rcx
; CHECK-NEXT:    callq dtor
define void @base_register() personality ptr @__gxx_personality_seh0 {
entry:
  %a = alloca i32, align 64
  call void @dtor(ptr %a)
  invoke void @may_throw() to label %cont unwind label %ehcleanup
cont:
  ret void
ehcleanup:
  %pad = cleanuppad within none []
  call void @dtor(ptr %a) [ "funclet"(token %pad) ]
  cleanupret from %pad unwind to caller
}

; The handler is entered with the callee-saved registers of the call; a value
; it reads that does not fit in one is in its slot while the funclet runs. The
; funclet reloads it from the establisher frame (the frame pointer), and what
; it spills is in its own frame (behind the frame pointer, in slots of its
; own). It allocates the outgoing arguments of its calls; an AArch64 funclet
; whose code modifies the link register saves that alone, above them.
; CHECK-LABEL: live_across:
; CHECK:         movq %rax, [[SLOT:[0-9]+]](%rsp) # 8-byte Spill
; CHECK:       $ehgcr_{{[0-9]+}}_
; CHECK-NEXT:    movq [[SLOT]](%rsp), %rax # 8-byte Reload
; CHECK:       "?dtor${{[0-9]+}}@?0?live_across@4HA":
; CHECK:         subq $88, %rsp
; CHECK-NEXT:    .seh_stackalloc 88
; CHECK-NEXT:    .seh_endprologue
; CHECK-NEXT:    movq [[SLOT]](%rdx), %rax # 8-byte Reload
; CHECK-NEXT:    movq %rax, 80(%rsp) # 8-byte Spill
; FP-LABEL: live_across:
; FP:         movq %rax, [[SLOT:-?[0-9]*]](%rbp) # 8-byte Spill
; FP:       $ehgcr_{{[0-9]+}}_
; FP-NEXT:    movq [[SLOT]](%rbp), %rax # 8-byte Reload
; FP:       "?dtor${{[0-9]+}}@?0?live_across@4HA":
; FP:         movq [[SLOT]](%rbp), %rax # 8-byte Reload
; FP-NOT:     %rax, [[SLOT]](%rbp)
; FP:         retq
; A64-LABEL: live_across:
; A64:         stur x8, [x29, #[[SLOT:-[0-9]+]]]
; A64:       $ehgcr_{{[0-9]+}}_
; A64-NEXT:    ldur x0, [x29, #[[SLOT]]]
; A64:       "?dtor${{[0-9]+}}@?0?live_across@4HA":
; A64:         str x30, [sp, #-16]!
; A64-NEXT:    .seh_save_reg_x x30, 16
; A64-NEXT:    sub sp, sp, #16
; A64-NEXT:    .seh_stackalloc 16
; A64-NEXT:    .seh_endprologue
; A64-NEXT:    ldur x8, [x29, #[[SLOT]]]
; A64-NOT:     stur x8, [x29, #[[SLOT]]]
; A64:         ret
define i64 @live_across(ptr %p) personality ptr @__gxx_personality_seh0 {
entry:
  %x = load i64, ptr %p
  call void asm sideeffect "", "~{rbx},~{rbp},~{rsi},~{rdi},~{r12},~{r13},~{r14},~{r15},~{x19},~{x20},~{x21},~{x22},~{x23},~{x24},~{x25},~{x26},~{x27},~{x28}"()
  invoke void @may_throw() to label %cont unwind label %ehcleanup
cont:
  ret i64 0
ehcleanup:
  %pad = cleanuppad within none []
  br label %loop
loop:
  %i = phi i64 [ %x, %ehcleanup ], [ %n, %loop ]
  call void @use9(ptr null, i64 0, i64 0, i64 0, i64 0, i64 0, i64 0, i64 0, i64 0) [ "funclet"(token %pad) ]
  call void asm sideeffect "", "~{rbx},~{rbp},~{rsi},~{rdi},~{r12},~{r13},~{r14},~{r15},~{x19},~{x20},~{x21},~{x22},~{x23},~{x24},~{x25},~{x26},~{x27},~{x28}"() [ "funclet"(token %pad) ]
  %n = add i64 %i, 1
  %c = icmp ult i64 %n, 4
  br i1 %c, label %loop, label %done
done:
  cleanupret from %pad unwind label %cs
cs:
  %s = catchswitch within none [label %catch] unwind to caller
catch:
  %cp = catchpad within %s [ptr null]
  catchret from %cp to label %body
body:
  ret i64 %x
}

; An object whose lifetime starts in a funclet and that the handler reads
; shares no memory with an object of the handler.
; CHECK-LABEL: lifetimes:
; CHECK:       $ehgcr_9_
; CHECK-NEXT:    leaq [[B:[0-9]+]](%rsp), %rcx
; CHECK-NEXT:    callq dtor
; CHECK-NEXT:    movl [[A:[0-9]+]](%rsp), %eax
; CHECK-NOT:     leaq [[B]](%rdx)
; CHECK:         leaq [[A]](%rdx), %rcx
; A64-LABEL: lifetimes:
; A64:       $ehgcr_9_
; A64-NEXT:    sub x0, x29, #[[B:[0-9]+]]
; A64-NEXT:    bl dtor
; A64-NEXT:    ldur w0, [x29, #-[[A:[0-9]+]]]
; A64-NOT:     sub x0, x29, #[[B]]
; A64:         sub x0, x29, #[[A]]
define i32 @lifetimes() personality ptr @__gxx_personality_seh0 {
entry:
  %a = alloca [16 x i32]
  %b = alloca [16 x i32]
  invoke void @may_throw() to label %cont unwind label %ehcleanup
cont:
  ret i32 0
ehcleanup:
  %pad = cleanuppad within none []
  call void @llvm.lifetime.start.p0(ptr %a)
  call void @dtor(ptr %a) [ "funclet"(token %pad) ]
  cleanupret from %pad unwind label %cs
cs:
  %s = catchswitch within none [label %catch] unwind to caller
catch:
  %cp = catchpad within %s [ptr null]
  catchret from %cp to label %body
body:
  call void @llvm.lifetime.start.p0(ptr %b)
  call void @dtor(ptr %b)
  call void @llvm.lifetime.end.p0(ptr %b)
  %v = load i32, ptr %a
  call void @llvm.lifetime.end.p0(ptr %a)
  ret i32 %v
}

; A call that the funclet makes on one path and then returns is a tail call
; on that path: the funclet's cleanupret is a return there too.
; CHECK-LABEL: conditional:
; CHECK:       "?dtor$2@?0?conditional@4HA":
; CHECK-NEXT:  .LBB10_2:
; CHECK-NEXT:    cmpb $0, 48(%rdx)
; CHECK-NEXT:    je .LBB10_3
; CHECK-NEXT:  # %bb.4:
; CHECK-NEXT:    movq 40(%rdx), %rcx
; CHECK-NEXT:    jmp free # TAILCALL
; CHECK-NEXT:  .LBB10_3:
; CHECK-NEXT:    retq # CLEANUPRET
define void @conditional() personality ptr @__gxx_personality_seh0 {
entry:
  %s = alloca { ptr, i8 }
  invoke void @may_throw() to label %cont unwind label %ehcleanup
cont:
  ret void
ehcleanup:
  %pad = cleanuppad within none []
  %f = getelementptr { ptr, i8 }, ptr %s, i64 0, i32 1
  %l = load i8, ptr %f
  %long = icmp ne i8 %l, 0
  br i1 %long, label %free, label %done
free:
  %p = load ptr, ptr %s
  call void @free(ptr %p) [ "funclet"(token %pad) ]
  br label %done
done:
  cleanupret from %pad unwind to caller
}

; A funclet's tail call passes its arguments in registers, so the function's
; incoming argument slots stay immutable, and a value read from one is read
; again where it is used rather than spilled.
; MIR-LABEL: name: stack_args
; MIR:       fixedStack:
; MIR-NEXT:    - { id: 0,
; MIR-NEXT:        isImmutable: true
define void @stack_args(i64, i64, i64, i64, i64 %e) personality ptr @__gxx_personality_seh0 {
entry:
  %a = alloca i32
  invoke void @may_throw() to label %cont unwind label %ehcleanup
cont:
  store volatile i64 %e, ptr @g
  ret void
ehcleanup:
  %pad = cleanuppad within none []
  call void @dtor(ptr %a) [ "funclet"(token %pad) ]
  cleanupret from %pad unwind to caller
}

; Only the handlers are EH continuation targets: a funclet is called.
; EHCONT:      .section .gehcont$y
; EHCONT-NEXT:   .symidx $ehgcr_1_{{[0-9]+}}
; EHCONT-NEXT:   .symidx $ehgcr_1_{{[0-9]+}}
; EHCONT-NEXT:   .symidx $ehgcr_2_{{[0-9]+}}
; EHCONT-NEXT:   .symidx $ehgcr_3_{{[0-9]+}}
; EHCONT-NEXT:   .symidx $ehgcr_8_{{[0-9]+}}
; EHCONT-NEXT:   .symidx $ehgcr_9_{{[0-9]+}}
; EHCONT-NOT:    .symidx

!llvm.module.flags = !{!0}
!0 = !{i32 1, !"ehcontguard", i32 1}

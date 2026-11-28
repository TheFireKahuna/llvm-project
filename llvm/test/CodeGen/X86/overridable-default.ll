; RUN: llc -mtriple=x86_64-unknown-windows-itanium -verify-machineinstrs < %s | FileCheck %s
; RUN: llc -mtriple=x86_64-unknown-windows-itanium -filetype=obj < %s \
; RUN:   | llvm-readobj --unwind - | FileCheck %s --check-prefix=UNWIND

; An overridable default begins with the check of its word, the image's
; __imp_ pointer of it, ahead of its unwind range, which with the exception
; tables begins past it. Where the word holds another function, the tail
; after the function compares the target's type prefix with the default's and
; transfers through the CFG dispatch; a System V variadic default, whose AL
; the dispatch's RAX would overwrite, through the check function, from a
; frame with unwind data of its own whose pops and jump are an epilogue. The
; own entry repeats the check and enters the body past it, or jumps through
; the word with the type checked. A replacement has no check.

declare void @may_throw()
declare i32 @__gxx_personality_seh0(...)

; CHECK-LABEL: {{^}}leaf:
; CHECK-NEXT:    leaq leaf(%rip), %r11
; CHECK-NEXT:    cmpq %r11, __imp_leaf(%rip)
; CHECK-NEXT:    jne [[TAIL:.Lovr_tail[0-9]+]]
; CHECK-NEXT:  [[BODY:.Lovr_body[0-9]+]]:
; CHECK-NEXT:  # %bb.0:
; CHECK-NEXT:    leaq 1(%rcx), %rax
; CHECK-NEXT:    retq
; CHECK:       [[TAIL]]:
; CHECK-NEXT:    movq __imp_leaf(%rip), %rax
; CHECK-NEXT:    movq -8(%r11), %r10
; CHECK-NEXT:    cmpq %r10, -8(%rax)
; CHECK-NEXT:    jne [[TRAP:.Lovr_trap[0-9]+]]
; CHECK-NEXT:    jmpq *__guard_dispatch_icall_fptr(%rip)
; CHECK-NEXT:  [[TRAP]]:
; CHECK-NEXT:    ud2
; CHECK-NEXT:    .p2align 4
; CHECK-NEXT:    .weak __ovr_own$leaf
; CHECK-NEXT:  __ovr_own$leaf:
; CHECK-NEXT:    leaq leaf(%rip), %r11
; CHECK-NEXT:    cmpq %r11, __imp_leaf(%rip)
; CHECK-NEXT:    je [[BODY]]
; CHECK-NEXT:    movq __imp_leaf(%rip), %r10
; CHECK-NEXT:    movq -8(%r11), %r11
; CHECK-NEXT:    cmpq %r11, -8(%r10)
; CHECK-NEXT:    jne [[TRAP]]
; CHECK-NEXT:    jmpq *%r10
define weak i64 @leaf(i64 %x) nounwind !kcfi_type !1 !overridable !2 {
  %r = add i64 %x, 1
  ret i64 %r
}

; CHECK-LABEL: {{^}}frame:
; CHECK:         jne [[TAIL:.Lovr_tail[0-9]+]]
; CHECK-NEXT:  [[BODY:.Lovr_body[0-9]+]]:
; CHECK-NEXT:  .seh_proc frame
; CHECK:       [[SITE:.Ltmp[0-9]+]]: # EH_LABEL
; CHECK-NEXT:    callq may_throw
; CHECK:         .uleb128 [[SITE]]-[[BODY]] # >> Call Site 1 <<
; CHECK:       [[TAIL]]:
; CHECK-NEXT:    movq __imp_frame(%rip), %rax
; UNWIND:      StartAddress: {{.*}} +0x10 (0x0)
; UNWIND-NEXT: EndAddress:
; UNWIND-NEXT: UnwindInfoAddress:
; UNWIND-NEXT: UnwindInfo {
; UNWIND-NEXT:   Version: 2
; UNWIND-NEXT:   Flags [
; UNWIND-NEXT:     ExceptionHandler
; UNWIND-NEXT:     TerminateHandler
; UNWIND-NEXT:   ]
; UNWIND-NEXT:   PrologSize: 4
define weak void @frame() personality ptr @__gxx_personality_seh0 !kcfi_type !1 !overridable !3 {
  invoke void @may_throw() to label %done unwind label %cs
cs:
  %s = catchswitch within none [label %catch] unwind to caller
catch:
  %cp = catchpad within %s [ptr null]
  catchret from %cp to label %done
done:
  ret void
}

; CHECK-LABEL: {{^}}untyped:
; CHECK:       .Lovr_tail{{[0-9]+}}:
; CHECK-NEXT:    movq __imp_untyped(%rip), %rax
; CHECK-NEXT:    jmpq *__guard_dispatch_icall_fptr(%rip)
; CHECK:       __ovr_own$untyped:
; CHECK-NEXT:    leaq untyped(%rip), %r11
; CHECK-NEXT:    cmpq %r11, __imp_untyped(%rip)
; CHECK-NEXT:    je .Lovr_body{{[0-9]+}}
; CHECK-NEXT:    jmpq *__imp_untyped(%rip)
define weak void @untyped() nounwind !overridable !4 {
  ret void
}

; CHECK-LABEL: {{^}}variadic:
; CHECK:       .Lovr_tail{{[0-9]+}}:
; CHECK-NEXT:    movq __imp_variadic(%rip), %r10
; CHECK-NEXT:    movq -8(%r11), %r11
; CHECK-NEXT:    cmpq %r11, -8(%r10)
; CHECK-NEXT:    jne [[TRAP:.Lovr_trap[0-9]+]]
; CHECK-NEXT:  [[CHECK:.Lovr_check[0-9]+]]:
; CHECK-NEXT:  .seh_proc [[CHECK]]
; CHECK-NEXT:    pushq %rax
; CHECK-NEXT:    .seh_pushreg %rax
; CHECK-NEXT:    pushq %rcx
; CHECK-NEXT:    .seh_pushreg %rcx
; CHECK-NEXT:    pushq %r10
; CHECK-NEXT:    .seh_pushreg %r10
; CHECK-NEXT:    .seh_endprologue
; CHECK-NEXT:    movq %r10, %rcx
; CHECK-NEXT:    callq *__guard_check_icall_fptr(%rip)
; CHECK-NEXT:    popq %r10
; CHECK-NEXT:    popq %rcx
; CHECK-NEXT:    popq %rax
; CHECK-NEXT:    rex64 jmpq *%r10
; CHECK-NEXT:    .seh_endproc
; CHECK-NEXT:  [[TRAP]]:
; UNWIND:      StartAddress: {{.*}}variadic{{.*}} +0x22
; UNWIND-NEXT: EndAddress: {{.*}}variadic{{.*}} +0x36
; UNWIND:        PrologSize: 4
; UNWIND:        UnwindCodes [
; UNWIND-NEXT:     0x04: PUSH_NONVOL reg=R10
; UNWIND-NEXT:     0x02: PUSH_NONVOL reg=RCX
; UNWIND-NEXT:     0x01: PUSH_NONVOL reg=RAX
; UNWIND-NEXT:   ]
define weak x86_64_sysvcc void @variadic(ptr %f, ...) nounwind !kcfi_type !1 !overridable !5 {
  ret void
}

; CHECK-LABEL: {{^}}replacement:
; CHECK-NEXT:  # %bb.0:
; CHECK-NEXT:    retq
; CHECK-NOT:   __ovr_own$replacement
define void @replacement() nounwind !kcfi_type !1 !overridable !{} {
  ret void
}

!llvm.module.flags = !{!6, !7, !8}
!1 = !{i32 286331153}
!2 = !{i32 0}
!3 = !{i32 3, i32 2, i32 1}
!4 = !{i32 8}
!5 = !{i32 20}
!6 = !{i32 4, !"kcfi", i32 1}
!7 = !{i32 1, !"function-type-prefix", i32 119298566}
!8 = !{i32 2, !"cfguard", i32 2}

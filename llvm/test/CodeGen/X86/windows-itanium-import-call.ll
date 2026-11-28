; DEFINE: %{x64} = llc -mtriple=x86_64-unknown-windows-itanium
; DEFINE: %{a64} = llc -mtriple=aarch64-pc-windows-ntposix
; RUN: %{x64} < %s | FileCheck %s --check-prefixes=X64,WI
; RUN: llc -mtriple=x86_64-pc-windows-ntposix < %s | FileCheck %s --check-prefixes=X64,NP
; RUN: %{x64} -mattr=+retpoline < %s | FileCheck %s --check-prefix=THUNK
; RUN: %{x64} -mattr=+slow-two-mem-ops,+slow-indirect-call < %s | FileCheck %s --check-prefix=X64
; RUN: llc -mtriple=x86_64-pc-windows-msvc < %s | FileCheck %s --check-prefix=MSVC
; RUN: %if aarch64-registered-target %{ %{a64} < %s | FileCheck %s --check-prefix=A64 %}
; RUN: %if aarch64-registered-target %{ llc -mtriple=aarch64-pc-windows-msvc < %s | FileCheck %s --check-prefix=A64-MSVC %}

; On Windows Itanium and NT-POSIX the linker makes a call through an import
; pointer direct where the callee is in the image, so each call loads the
; pointer for itself, on x86-64 directly before it whatever the tuning, unless
; optimized for minimum size or cold. Other Windows targets share one load.

declare i32 @chosen(i32) nonlazybind
declare dllimport i32 @known(i32)

define i32 @calls(i32 %x) {
; X64-LABEL: calls:
; X64-NOT:        __imp_
; X64:            movq __imp_chosen@LOAD_CALL(%rip), %rax
; X64-NEXT:       callq *%rax
; X64-NEXT:       movl %eax, %e{{cx|di}}
; X64-NEXT:       movq __imp_chosen@LOAD_CALL(%rip), %rax
; X64-NEXT:       callq *%rax
; X64-NEXT:       movl %eax, %e{{cx|di}}
; X64-NEXT:       movq __imp_known@LOAD_CALL(%rip), %rax
; X64-NEXT:       callq *%rax
; X64-NEXT:       movl %eax, %e{{cx|di}}
; X64-NEXT:       movq __imp_known@LOAD_CALL(%rip), %rax
; X64-NEXT:       callq *%rax
; X64-NEXT:       movl %eax, %e{{cx|di}}
; X64-NOT:        __imp_
; X64:            jmpq *__imp_chosen(%rip)
; THUNK-LABEL: calls:
; THUNK:          movq __imp_chosen(%rip), %rsi
; THUNK-NEXT:     movq %rsi, %r11
; THUNK-NEXT:     callq __llvm_retpoline_r11
; THUNK-NEXT:     movl %eax, %ecx
; THUNK-NEXT:     movq %rsi, %r11
; THUNK-NEXT:     callq __llvm_retpoline_r11
; MSVC-LABEL: calls:
; MSVC:           movq __imp_known(%rip), %rsi
; MSVC:           callq *%rsi
; MSVC:           callq *%rsi
; A64-LABEL: calls:
; A64-NOT:        x19
; A64:            adrp x[[A:[0-9]+]], __imp_chosen
; A64-NEXT:       ldr x[[A]], [x[[A]], :fused_lo12:__imp_chosen]
; A64:            blr x[[A]]
; A64-NEXT:       adrp x[[B:[0-9]+]], __imp_chosen
; A64-NEXT:       ldr x[[B]], [x[[B]], :fused_lo12:__imp_chosen]
; A64:            blr x[[B]]
; A64-NEXT:       adrp x[[C:[0-9]+]], __imp_known
; A64-NEXT:       ldr x[[C]], [x[[C]], :fused_lo12:__imp_known]
; A64:            blr x[[C]]
; A64-NEXT:       adrp x[[D:[0-9]+]], __imp_known
; A64-NEXT:       ldr x[[D]], [x[[D]], :fused_lo12:__imp_known]
; A64:            blr x[[D]]
; A64-NEXT:       adrp x[[E:[0-9]+]], __imp_chosen
; A64-NEXT:       ldr x[[E]], [x[[E]], :fused_lo12:__imp_chosen]
; A64:            br x[[E]]
; A64-MSVC-LABEL: calls:
; A64-MSVC:       ldr x19, [x19, :lo12:__imp_known]
; A64-MSVC:       blr x19
; A64-MSVC:       blr x19
  %a = call i32 @chosen(i32 %x)
  %b = call i32 @chosen(i32 %a)
  %c = call i32 @known(i32 %b)
  %d = call i32 @known(i32 %c)
  %e = tail call i32 @chosen(i32 %d)
  ret i32 %e
}

define void @loop(i32 %n) {
; X64-LABEL: loop:
; X64:            movq __imp_chosen@LOAD_CALL(%rip), %rax
; X64-NEXT:       callq *%rax
; X64-NOT:        __imp_chosen
; X64:          [[LOOP:.LBB[0-9_]+]]:
; X64:            movq __imp_chosen@LOAD_CALL(%rip), %rax
; X64-NEXT:       callq *%rax
; X64-NEXT:       movl
; X64-NEXT:       movq __imp_chosen@LOAD_CALL(%rip), %rax
; X64-NEXT:       callq *%rax
; X64:            jne [[LOOP]]
; X64:            jmpq *__imp_chosen(%rip)
; A64-LABEL: loop:
; A64:          [[LOOP:.LBB[0-9_]+]]:
; A64:            ldr x[[P:[0-9]+]], [x[[P]], :fused_lo12:__imp_chosen]
; A64:            blr x[[P]]
; A64:            ldr x[[R:[0-9]+]], [x[[R]], :fused_lo12:__imp_chosen]
; A64:            blr x[[R]]
; A64:            b.ne [[LOOP]]
; A64:            ldr x[[Q:[0-9]+]], [x[[Q]], :fused_lo12:__imp_chosen]
; A64:            br x[[Q]]
entry:
  %0 = call i32 @chosen(i32 %n)
  %c = icmp sgt i32 %n, 0
  br i1 %c, label %body, label %exit
body:
  %i = phi i32 [ 0, %entry ], [ %inc, %body ]
  %1 = call i32 @chosen(i32 %i)
  %2 = call i32 @chosen(i32 %i)
  %inc = add i32 %i, 1
  %d = icmp eq i32 %inc, %n
  br i1 %d, label %exit, label %body
exit:
  %3 = tail call i32 @chosen(i32 0)
  ret void
}

; A tail call through its pointer ends the block that restores the frame, as a
; direct one does, so only the path that calls sets the frame up.

declare void @sink(ptr) nonlazybind

define void @guarded(ptr %p) {
; X64-LABEL: guarded:
; X64-NOT:        pushq
; X64:            je .LBB2_1
; X64:            pushq %r[[S:(si|bx)]]
; X64:            movq __imp_chosen@LOAD_CALL(%rip), %rax
; X64-NEXT:       callq *%rax
; X64:            popq %r[[S]]
; X64-NOT:        retq
; X64:            jmpq *__imp_sink(%rip)
entry:
  %c = icmp eq ptr %p, null
  br i1 %c, label %exit, label %do
do:
  %0 = call i32 @chosen(i32 0)
  tail call void @sink(ptr %p)
  br label %exit
exit:
  ret void
}

; A function that is not optimized takes the same forms: no load is folded
; when its instructions are selected, so each goes into its call or tail call
; where that is emitted.
define i32 @unoptimized(i32 %x) optnone noinline {
; X64-LABEL: unoptimized:
; X64:            movq __imp_chosen@LOAD_CALL(%rip), %rax
; X64-NEXT:       callq *%rax
; X64:            {{^}} jmpq *__imp_chosen(%rip)
  %a = call i32 @chosen(i32 %x)
  %b = musttail call i32 @chosen(i32 %a)
  ret i32 %b
}

; A virtual call loads its target into the register its table was in, and a
; call through any other pointer in memory into one it leaves free. NT-POSIX
; passes the number of vector registers of a variadic call in AL.
@fp = external dso_local global ptr
declare void @variadic(i32, ...) nonlazybind

define void @memory(ptr %o) {
; X64-LABEL: memory:
; X64:            movq (%r{{cx|di}}), %rax
; X64-NEXT:       movq 16(%rax), %rax
; X64-NEXT:       callq *%rax
; X64-NEXT:       movq fp(%rip), %rax
; X64-NEXT:       callq *%rax
; WI:             movq __imp_variadic@LOAD_CALL(%rip), %rax
; WI-NEXT:        callq *%rax
; NP:             xorl %eax, %eax
; NP-NEXT:        movq __imp_variadic@LOAD_CALL(%rip), %rcx
; NP-NEXT:        callq *%rcx
; MSVC-LABEL: memory:
; MSVC:           callq *16(%rax)
; MSVC:           callq *fp(%rip)
  %vt = load ptr, ptr %o
  %slot = getelementptr ptr, ptr %vt, i64 2
  %f = load ptr, ptr %slot
  call void %f(ptr %o)
  %g = load ptr, ptr @fp
  call void %g()
  call void (i32, ...) @variadic(i32 1)
  ret void
}

define void @sized(ptr %o) optsize {
; X64-LABEL: sized:
; X64:            movq 16(%rax), %rax
; X64-NEXT:       callq *%rax
; X64-NEXT:       xorl
; X64-NEXT:       movq __imp_chosen@LOAD_CALL(%rip), %rax
; X64-NEXT:       callq *%rax
  %vt = load ptr, ptr %o
  %slot = getelementptr ptr, ptr %vt, i64 2
  %f = load ptr, ptr %slot
  call void %f(ptr %o)
  %a = call i32 @chosen(i32 0)
  ret void
}

define void @smallest(ptr %o) minsize {
; X64-LABEL: smallest:
; X64:            callq *16(%rax)
; X64-NEXT:       xorl
; X64-NEXT:       callq *__imp_chosen(%rip)
  %vt = load ptr, ptr %o
  %slot = getelementptr ptr, ptr %vt, i64 2
  %f = load ptr, ptr %slot
  call void %f(ptr %o)
  %a = call i32 @chosen(i32 0)
  ret void
}

define void @rare(ptr %o) cold {
; X64-LABEL: rare:
; X64:            callq *16(%rax)
; X64-NEXT:       xorl
; X64-NEXT:       callq *__imp_chosen(%rip)
  %vt = load ptr, ptr %o
  %slot = getelementptr ptr, ptr %vt, i64 2
  %f = load ptr, ptr %slot
  call void %f(ptr %o)
  %a = call i32 @chosen(i32 0)
  ret void
}

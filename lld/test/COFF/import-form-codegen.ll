; REQUIRES: x86, aarch64
;; Compiled import forms meet the link: calls, tail calls, address-takes and a
;; loop's fused call bind direct where the function is in the image, and stay
;; through the pointer where it is imported; an absent weak pointer is zero.
; RUN: rm -rf %t && split-file %s %t && cd %t
; DEFINE: %{triple} = x86_64-unknown-windows-itanium
; DEFINE: %{build} = llc -mtriple=%{triple} -filetype=obj use.ll -o use.obj && \
; DEFINE:   llc -mtriple=%{triple} -filetype=obj def.ll -o def.obj && \
; DEFINE:   llvm-mc -triple=%{triple} -filetype=obj \
; DEFINE:     %S/Inputs/loadconfig-cfg-iat.s -o config.obj && \
; DEFINE:   lld-link -dll -noentry def.obj -out:def.dll -implib:def.lib \
; DEFINE:     -export:called -export:addressed -export:imported && \
; DEFINE:   lld-link -dll -noentry use.obj def.obj -out:local.dll -export:call \
; DEFINE:     -export:tail -export:address -export:maybe -export:loop \
; DEFINE:     -export:called -export:addressed && \
; DEFINE:   lld-link -dll -noentry use.obj def.lib -out:imported.dll \
; DEFINE:     -export:main -export:loop && \
; DEFINE:   lld-link -dll -noentry -guard:cf def.obj use.obj config.obj \
; DEFINE:     -out:guard.dll -export:called -export:addressed
; DEFINE: %{dump} = llvm-objdump -d --no-show-raw-insn
; RUN: %{build}
; RUN: %{dump} local.dll | FileCheck %s --check-prefix=X64 \
; RUN:   --implicit-check-not=__imp_
; RUN: %{dump} imported.dll | FileCheck %s --check-prefix=X64-IMP
; RUN: llvm-readobj --coff-load-config guard.dll | FileCheck %s --check-prefix=GUARD
; REDEFINE: %{triple} = aarch64-pc-windows-ntposix
; RUN: %{build}
; RUN: %{dump} local.dll | FileCheck %s --check-prefix=A64
; RUN: %{dump} imported.dll | FileCheck %s --check-prefix=A64-IMP
; RUN: llvm-readobj --coff-load-config guard.dll | FileCheck %s --check-prefix=GUARD

;; Each of two calls reads the pointer for itself, so both are direct, five
;; bytes on x86-64; a tail call that a branch jumps over is the opposite branch.
; X64-LABEL: <call>:
; X64-NEXT:    subq
; X64-NEXT:    callq {{.*}} <called>
; X64-NEXT:    callq {{.*}} <called>
; X64-NEXT:    addq
; X64-LABEL: <tail>:
; X64-NEXT:    jmp {{.*}} <called>
; X64-LABEL: <address>:
; X64-NEXT:    leaq {{.*}} <addressed>
; X64-LABEL: <loop>:
; X64-NEXT:    pushq %rsi
; X64-NEXT:    subq
; X64:         [[#%x,C:]]: callq {{.*}} <called>
; X64-NEXT:    [[#C+5]]: decl %esi
; X64-NEXT:    jne 0x[[#%x,C]]
; X64-LABEL: <maybe>:
; X64-NEXT:    testb
; X64-NEXT:    jne {{.*}} <called>
; X64-NEXT:    retq

; A64-LABEL: <address>:
; A64-NEXT:    adr x0, {{.*}} <addressed>
; A64-LABEL: <loop>:
; A64-NOT:     {{ldr x8|blr}}
; A64:         [[#%x,CALL:]]: bl
; A64-NEXT:    subs w19, w19, #0x1
; A64-NEXT:    b.ne 0x[[#CALL]]

;; Imported, the loop's load and call stay (the AArch64 adrp folded into the
;; load); the weak pointer's load is zero, its test gone and the call skipped.
; X64-IMP-LABEL: <loop>:
; X64-IMP:         [[#%x,L:]]: movq {{.*}}(%rip), %rax
; X64-IMP-NEXT:    callq *%rax
; X64-IMP-NEXT:    decl %esi
; X64-IMP-NEXT:    jne 0x[[#%x,L]]
; X64-IMP-LABEL: <main>:
; X64-IMP:         movq $0x0, %rax
; X64-IMP-NEXT:    movq %rax, {{.*}}(%rip)
; X64-IMP-NEXT:    jmp
; X64-IMP-NEXT:    movq $0x0, %rax
; X64-IMP-NEXT:    callq *%rax
; X64-IMP-NEXT:    movq {{.*}}(%rip), %rax
; X64-IMP-NEXT:    movq %rax, {{.*}}(%rip)
; X64-IMP-NEXT:    movq {{.*}}(%rip), %rax
; X64-IMP-NEXT:    callq *%rax
; X64-IMP-NEXT:    movq {{.*}}(%rip), %rax
; X64-IMP-NEXT:    callq *%rax

; A64-IMP-LABEL: <loop>:
; A64-IMP:         [[#%x,LOAD:]]: ldr x8, 0x[[#%x,]]
; A64-IMP-NEXT:    blr x8
; A64-IMP-NEXT:    subs w19, w19, #0x1
; A64-IMP-NEXT:    b.ne 0x[[#LOAD]]
; A64-IMP-LABEL: <main>:
; A64-IMP:         mov x8, #0x0
; A64-IMP-NEXT:    str x8, [x19]
; A64-IMP-NEXT:    b 0x
; A64-IMP-NEXT:    blr x8
; A64-IMP-NEXT:    ldr x8, 0x
; A64-IMP-NEXT:    str x8, [x19]
; A64-IMP-NEXT:    blr x8
; A64-IMP-NEXT:    ldr x8, 0x
; A64-IMP-NEXT:    blr x8

;; addressed and imported, whose addresses the code takes, are call targets
;; with no pointer left; called, only called, is listed as an export, suppressed.
; GUARD:      GuardFidTable [
; GUARD-NEXT:   0x180001000 flags 2
; GUARD-NEXT:   0x180001010{{$}}
; GUARD-NEXT:   0x180001020{{$}}
; GUARD-NEXT: ]

;--- use.ll
declare void @called() nonlazybind
declare void @addressed()
declare extern_weak void @weak()
declare dllimport void @imported()
@slot = global ptr null

define void @call() {
  call void @called()
  call void @called()
  ret void
}

define void @tail() {
  tail call void @called()
  ret void
}

define ptr @address() {
  ret ptr @addressed
}

define void @loop(i32 %n) {
entry:
  br label %body
body:
  %i = phi i32 [ 0, %entry ], [ %next, %body ]
  call void @called()
  %next = add i32 %i, 1
  %done = icmp eq i32 %next, %n
  br i1 %done, label %exit, label %body
exit:
  ret void
}

;; In a section of its own, whose resizing leaves maybe's alone.
define void @main() section ".text$main" {
entry:
  store ptr @weak, ptr @slot
  %c = icmp ne ptr @weak, null
  br i1 %c, label %call, label %exit
call:
  call void @weak()
  br label %exit
exit:
  store ptr @imported, ptr @slot
  call void @imported()
  call void @imported()
  ret void
}

;; Last, so that its tail call reaches the next chunk, called.
define void @maybe(i1 %c) {
  br i1 %c, label %then, label %else
then:
  tail call void @called()
  ret void
else:
  ret void
}

;--- def.ll
define void @called() align 16 {
  ret void
}

define void @addressed() align 16 {
  ret void
}

define void @imported() align 16 {
  ret void
}

; RUN: llc < %s -mtriple=x86_64-unknown-windows-itanium | \
; RUN:   FileCheck %s --check-prefixes=CHECK,FUSED
; RUN: llc < %s -mtriple=x86_64-pc-windows-ntposix | \
; RUN:   FileCheck %s --check-prefixes=CHECK,FUSED
; RUN: llc < %s -mtriple=x86_64-pc-windows-msvc | \
; RUN:   FileCheck %s --check-prefixes=CHECK,PLAIN --implicit-check-not=FUSED \
; RUN:   --implicit-check-not=int3
; RUN: %if aarch64-registered-target %{ llc < %s -mtriple=aarch64-unknown-windows-itanium | FileCheck %s --check-prefixes=A64,A64F %}
; RUN: %if aarch64-registered-target %{ llc < %s -mtriple=aarch64-pc-windows-ntposix | FileCheck %s --check-prefixes=A64,A64F %}
; RUN: %if aarch64-registered-target %{ llc < %s -mtriple=aarch64-pc-windows-msvc | \
; RUN:   FileCheck %s --check-prefixes=A64,A64P --implicit-check-not=fused --implicit-check-not=FUSED %}

; On Windows Itanium and NT-POSIX a call or tail call through a register that
; only a load of an import pointer reaches is a fused call or jump naming the
; pointer, and the load a fused read. On x86-64 each call loads its pointer
; directly before it and a tail call jumps through the pointer in memory.

declare dllimport void @imported()
declare dllimport void @taking(ptr)
declare extern_weak void @weak()

; The load in a loop's preheader reaches the call in the loop alone.
define void @loop(i32 %n) {
; A64:         adrp x20, __imp_imported
; A64F:          ldr x20, [x20, :fused_lo12:__imp_imported]
; A64P:          ldr x20, [x20, :lo12:__imp_imported]
; A64:         // %body
; A64-NOT:       adrp
; A64F:        [[SITE:.Ltmp[0-9]+]]:
; A64F-NEXT:     .reloc [[SITE]], IMAGE_REL_LLVM_FUSED_CALL, __imp_imported
; A64:           blr x20
entry:
  br label %body

body:
  %i = phi i32 [ 0, %entry ], [ %next, %body ]
  call void @imported()
  %next = add i32 %i, 1
  %done = icmp eq i32 %next, %n
  br i1 %done, label %exit, label %body

exit:
  ret void
}

; A tail call through the register is a fused jump.
define void @tail() optnone noinline {
; A64-LABEL: tail:
; A64F:          ldr x0, [x0, :fused_lo12:__imp_imported]
; A64F-NEXT:   [[SITE:.Ltmp[0-9]+]]:
; A64F-NEXT:     .reloc [[SITE]], IMAGE_REL_LLVM_FUSED_JUMP, __imp_imported
; A64F-NEXT:     br x0
  musttail call void @imported()
  ret void
}

; A store reads the register, so the load stays a load, which alone reaches
; the call after it, a fused call still on AArch64. The next call loads for
; itself, as on x86-64 both do.
define void @stored(ptr %p) {
; CHECK-LABEL: stored:
; CHECK:         movq __imp_imported(%rip), [[REG:%r[a-z0-9]+]]
; CHECK-NEXT:    movq [[REG]], (%r{{[a-z]+}})
; FUSED-NEXT:    movq __imp_imported@LOAD_CALL(%rip), %rax
; FUSED-NEXT:    callq *%rax
; FUSED-NEXT:    movq __imp_imported@LOAD_CALL(%rip), %rax
; FUSED-NEXT:    callq *%rax
; PLAIN-NEXT:    callq *[[REG]]
; PLAIN-NEXT:    callq *[[REG]]
; A64-LABEL: stored:
; A64:           ldr [[REG:x[0-9]+]], [x{{[0-9]+}}, :lo12:__imp_imported]
; A64-NEXT:      str [[REG]], [x0]
; A64F-NEXT:   [[SITE:.Ltmp[0-9]+]]:
; A64F-NEXT:     .reloc [[SITE]], IMAGE_REL_LLVM_FUSED_CALL, __imp_imported
; A64-NEXT:      blr [[REG]]
; A64F-NEXT:     adrp x8, __imp_imported
; A64F-NEXT:     ldr x8, [x8, :fused_lo12:__imp_imported]
; A64F-NEXT:   [[SITE:.Ltmp[0-9]+]]:
; A64F-NEXT:     .reloc [[SITE]], IMAGE_REL_LLVM_FUSED_CALL, __imp_imported
; A64F-NEXT:     blr x8
; A64P-NEXT:     blr [[REG]]
  store ptr @imported, ptr %p
  call void @imported()
  call void @imported()
  ret void
}

; On AArch64 a comparison after the calls reads the first call's register.
define i1 @compared(ptr %p) {
; A64-LABEL: compared:
; A64:           ldr x20, [x20, :lo12:__imp_imported]
; A64F-NEXT:   [[SITE:.Ltmp[0-9]+]]:
; A64F-NEXT:     .reloc [[SITE]], IMAGE_REL_LLVM_FUSED_CALL, __imp_imported
; A64-NEXT:      blr x20
; A64F-NEXT:     adrp x8, __imp_imported
; A64F-NEXT:     ldr x8, [x8, :fused_lo12:__imp_imported]
; A64F-NEXT:   [[SITE:.Ltmp[0-9]+]]:
; A64F-NEXT:     .reloc [[SITE]], IMAGE_REL_LLVM_FUSED_CALL, __imp_imported
; A64F-NEXT:     blr x8
; A64P-NEXT:     blr x20
; A64-NEXT:      cmp x19, x20
  call void @imported()
  call void @imported()
  %c = icmp eq ptr %p, @imported
  ret i1 %c
}

; The calls are also reached with what the function was given in the register,
; so each stays a call.
define void @merged(i1 %c, ptr %p) {
; CHECK-LABEL: merged:
; CHECK:         movq __imp_imported(%rip), [[REG:%r[a-z0-9]+]]
; CHECK:         movq %r{{[a-z]+}}, [[REG]]
; CHECK:       # %call
; CHECK-NEXT:    callq *[[REG]]
; CHECK-NEXT:    callq *[[REG]]
; A64-LABEL: merged:
; A64:           ldr x19, [x19, :lo12:__imp_imported]
; A64:         // %call
; A64-NEXT:      blr x19
; A64-NEXT:      blr x19
  br i1 %c, label %then, label %else

then:
  br label %call

else:
  store i32 0, ptr %p
  br label %call

call:
  %f = phi ptr [ @imported, %then ], [ %p, %else ]
  call void %f()
  call void %f()
  ret void
}

; A call loads the pointer and a tail call jumps through it in memory. Elsewhere a
; copy to the register of the tail call reads the register of the call, and
; the copy reaches the tail call, a jmp like any other. On AArch64 the
; epilogue lies between the load and the tail call, and writes another
; register.
define void @call_then_tail() {
; CHECK-LABEL: call_then_tail:
; FUSED:         movq __imp_imported@LOAD_CALL(%rip), %rax
; FUSED-NEXT:    callq *%rax
; FUSED:         {{^}} jmpq *__imp_imported(%rip)
; PLAIN:         movq __imp_imported(%rip), [[REG:%r[a-z0-9]+]]
; PLAIN:         callq *[[REG]]
; PLAIN-NEXT:    movq [[REG]], %rax
; PLAIN:         rex64 jmpq *%rax
; A64-LABEL: call_then_tail:
; A64:           blr x{{[08]}}
; A64-NEXT:      adrp x0, __imp_imported
; A64F-NEXT:     ldr x0, [x0, :fused_lo12:__imp_imported]
; A64:           ldr x30, [sp], #16
; A64F:        [[SITE:.Ltmp[0-9]+]]:
; A64F-NEXT:     .reloc [[SITE]], IMAGE_REL_LLVM_FUSED_JUMP, __imp_imported
; A64:           br x0
  call void @imported()
  tail call void @imported()
  ret void
}

; On AArch64 a call through a pointer goes through a register that still
; holds the pointer on every path to it, here the one that the test of a weak
; function read, and so does a tail call. On x86-64 the call loads the pointer
; anew and the tail call jumps through it in memory.
define void @held(ptr %p) {
; CHECK-LABEL: held:
; FUSED:         movq __imp_weak(%rip), %rax
; PLAIN:         movq .refptr.weak(%rip), %rax
; CHECK-NEXT:    movq %rax, (%r{{[a-z]+}})
; FUSED-NEXT:  [[TEST:.Ltmp[0-9]+]]:
; FUSED-NEXT:    .reloc [[TEST]], IMAGE_REL_LLVM_FUSED_TEST_REG, __imp_weak
; CHECK-NEXT:    testq %rax, %rax
; FUSED:         movq __imp_weak@LOAD_CALL(%rip), %rax
; FUSED-NEXT:    callq *%rax
; PLAIN:         callq *.refptr.weak(%rip)
; A64-LABEL: held:
; A64F:          ldr x8, [x8, :lo12:__imp_weak]
; A64P:          ldr x8, [x8, :lo12:.refptr.weak]
; A64-NEXT:      str x8, [x0]
; A64F-NEXT:   [[TEST:.Ltmp[0-9]+]]:
; A64F-NEXT:     .reloc [[TEST]], IMAGE_REL_LLVM_FUSED_TEST, __imp_weak
; A64-NEXT:      cbz x8,
; A64F:        [[SITE:.Ltmp[0-9]+]]:
; A64F-NEXT:     .reloc [[SITE]], IMAGE_REL_LLVM_FUSED_CALL, __imp_weak
; A64:           blr x8
entry:
  store ptr @weak, ptr %p
  %c = icmp ne ptr @weak, null
  br i1 %c, label %call, label %exit

call:
  call void @weak()
  br label %exit

exit:
  ret void
}

define void @held_tail(ptr %p) {
; CHECK-LABEL: held_tail:
; CHECK:         testq %rax, %rax
; CHECK-NEXT:    je
; FUSED:         {{^}} jmpq *__imp_weak(%rip)
; PLAIN:         jmpq *.refptr.weak(%rip)
; A64-LABEL: held_tail:
; A64:           cbz x1,
; A64F:        [[SITE:.Ltmp[0-9]+]]:
; A64F-NEXT:     .reloc [[SITE]], IMAGE_REL_LLVM_FUSED_JUMP, __imp_weak
; A64:           br x1
entry:
  store ptr @weak, ptr %p
  %c = icmp ne ptr @weak, null
  br i1 %c, label %call, label %exit

call:
  tail call void @weak()
  ret void

exit:
  ret void
}

; A call before it writes the register, so each call loads its pointer, on
; AArch64 a fused read; elsewhere a register that calls preserve holds the
; weak pointer.
define void @held_no_more(ptr %p) {
; CHECK-LABEL: held_no_more:
; FUSED:         movq __imp_imported@LOAD_CALL(%rip), %rax
; FUSED-NEXT:    callq *%rax
; FUSED-NEXT:    movq __imp_weak@LOAD_CALL(%rip), %rax
; FUSED-NEXT:    callq *%rax
; PLAIN:         callq *__imp_imported(%rip)
; PLAIN-NEXT:    callq *.refptr.weak(%rip)
; A64-LABEL: held_no_more:
; A64F:          cbz x8,
; A64F:          ldr x8, [x8, :fused_lo12:__imp_imported]
; A64F-NEXT:   [[SITE:.Ltmp[0-9]+]]:
; A64F-NEXT:     .reloc [[SITE]], IMAGE_REL_LLVM_FUSED_CALL, __imp_imported
; A64F-NEXT:     blr x8
; A64F-NEXT:     adrp x8, __imp_weak
; A64F-NEXT:     ldr x8, [x8, :fused_lo12:__imp_weak]
; A64F-NEXT:   [[SITE:.Ltmp[0-9]+]]:
; A64F-NEXT:     .reloc [[SITE]], IMAGE_REL_LLVM_FUSED_CALL, __imp_weak
; A64F-NEXT:     blr x8
; A64P:          cbz x19,
; A64P:          blr x8
; A64P-NEXT:     blr x19
entry:
  store ptr @weak, ptr %p
  %c = icmp ne ptr @weak, null
  br i1 %c, label %call, label %exit

call:
  call void @imported()
  call void @weak()
  br label %exit

exit:
  ret void
}

; On AArch64, branch target enforcement chooses the register.
; A64-LABEL: tail_bti:
; A64:           adrp x16, __imp_imported
; A64F-NEXT:     ldr x16, [x16, :fused_lo12:__imp_imported]
; A64F-NEXT:   [[SITE:.Ltmp[0-9]+]]:
; A64F-NEXT:     .reloc [[SITE]], IMAGE_REL_LLVM_FUSED_JUMP, __imp_imported
; A64:           br x16
define void @tail_bti() "branch-target-enforcement" {
  tail call void @imported()
  ret void
}

; The callee reads the register as its argument, so the load stays a load.
; A64-LABEL: tail_argument:
; A64:           ldr x0, [x0, :lo12:__imp_taking]
; A64-NEXT:      br x0
define void @tail_argument() {
  tail call void @taking(ptr @taking)
  ret void
}

; RUN: llc -mtriple=x86_64-unknown-windows-itanium < %s | FileCheck %s --check-prefixes=CHECK,WI
; RUN: llc -mtriple=x86_64-unknown-windows-ntposix < %s | FileCheck %s
; RUN: sed 's/no-unwind-version/winx64-eh-unwind/' %s \
; RUN:   | not llc -mtriple=x86_64-unknown-windows-itanium 2>&1 | FileCheck %s --check-prefix=V3

; Version 2 unwind data places each pop by the plain instruction's length, so
; only the last pop may be longer: a frame keeps the paired push (PPX) for the
; register it pushes first and saves the others by plain pushes, never a push
; of two registers first, the stack being 8 bytes off 16 at entry.

declare void @ext(ptr)
declare void @exti(i64)

; With no frame register that is the first callee-saved register.
define void @no_frame_register() "target-features"="+ppx" uwtable {
; CHECK-LABEL: no_frame_register:
; CHECK:         .seh_unwindversion 2
; CHECK-NEXT:    pushp %r14
; CHECK-NEXT:    .seh_pushreg %r14
; CHECK-NEXT:    pushq %r12
; CHECK-NEXT:    .seh_pushreg %r12
; CHECK-NEXT:    pushq %rbx
; CHECK:         .seh_unwindv2start
; CHECK-NEXT:    popq %rbx
; CHECK-NEXT:    popq %r12
; CHECK-NEXT:    popp %r14
; CHECK-NEXT:    .seh_endepilogue
; CHECK-NEXT:    retq
  call void asm sideeffect "", "~{rbx},~{r12},~{r14}"()
  call void @exti(i64 0)
  ret void
}

; The frame register is pushed last and restored by `leave`, a plain push
; with no pop, so the function still ends in `leave` and the pops.
define void @leave() "frame-pointer"="all" "target-features"="+ppx" uwtable {
; CHECK-LABEL: leave:
; CHECK:         pushp %rbx
; CHECK-NEXT:    .seh_pushreg %rbx
; CHECK-NEXT:    pushq %rbp
; CHECK-NEXT:    .seh_stackalloc 8, large
; CHECK:         .seh_startepilogue
; CHECK-NEXT:    leave
; CHECK-NEXT:    .seh_unwindv2start
; CHECK-NEXT:    popp %rbx
; CHECK-NEXT:    .seh_endepilogue
; CHECK-NEXT:    retq
  %b = alloca [24 x i8]
  call void asm sideeffect "", "~{rbx}"()
  call void @ext(ptr %b)
  ret void
}

; So are the reloads of the XMM registers ahead of it.
define void @leave_xmm(i64 %n) "target-features"="+ppx" uwtable {
; WI-LABEL:    leave_xmm:
; WI:            pushp %r12
; WI:            pushq %rbx
; WI:            pushq %rbp
; WI:            movaps -16(%rbp), %xmm6
; WI-NEXT:       .seh_startepilogue
; WI-NEXT:       leave
; WI-NEXT:       .seh_unwindv2start
; WI-NEXT:       popq %rbx
; WI-NEXT:       popp %r12
; WI-NEXT:       .seh_endepilogue
  %b = alloca i8, i64 %n
  call void asm sideeffect "", "~{rbx},~{r12},~{xmm6}"()
  call void @ext(ptr %b)
  ret void
}

; Where the frame register is set toward the middle of the frame it is pushed
; first and popped last.
define void @frame_register_first() "frame-pointer"="all" "target-features"="+ppx" uwtable {
; CHECK-LABEL: frame_register_first:
; CHECK:         pushp %rbp
; CHECK-NEXT:    .seh_pushreg %rbp
; CHECK-NEXT:    pushq %rbx
; CHECK:         .seh_unwindv2start
; CHECK-NEXT:    popq %rbx
; CHECK-NEXT:    popp %rbp
; CHECK-NEXT:    .seh_endepilogue
  %b = alloca [400 x i8]
  call void asm sideeffect "", "~{rbx}"()
  call void @ext(ptr %b)
  ret void
}

; No register is pushed with another, with the paired form or without it.
define void @pairs() "target-features"="+push2pop2,+ppx" uwtable {
; CHECK-LABEL: pairs:
; CHECK-NOT:     push2
; CHECK:         pushp %r15
; CHECK-NEXT:    .seh_pushreg %r15
; CHECK-NEXT:    pushq %r14
; CHECK:         .seh_unwindv2start
; CHECK-NEXT:    popq %rbx
; CHECK-NEXT:    popq %r12
; CHECK-NEXT:    popq %r13
; CHECK-NEXT:    popq %r14
; CHECK-NEXT:    popp %r15
; CHECK-NEXT:    .seh_endepilogue
  call void asm sideeffect "", "~{rbx},~{r12},~{r13},~{r14},~{r15}"()
  call void @exti(i64 0)
  ret void
}

define void @no_ppx() "target-features"="+push2pop2" uwtable {
; CHECK-LABEL: no_ppx:
; CHECK-NOT:     push2
; CHECK:         pushq %r15
; CHECK-NEXT:    .seh_pushreg %r15
; CHECK:         .seh_unwindv2start
; CHECK-NEXT:    popq %rbx
; CHECK-NEXT:    popq %r12
; CHECK-NEXT:    popq %r13
; CHECK-NEXT:    popq %r14
; CHECK-NEXT:    popq %r15
; CHECK-NEXT:    .seh_endepilogue
  call void asm sideeffect "", "~{rbx},~{r12},~{r13},~{r14},~{r15}"()
  call void @exti(i64 0)
  ret void
}

; Version 2 names a saved register in four bits. r16 to r29 are volatile, and
; r30 and r31, which Windows makes nonvolatile, are reserved, so no function
; saves a register the data cannot name. A module that asks for version 3,
; which no supported Windows unwinds, is an error.
define i64 @egpr(i64 %a, i64 %b) "target-features"="+egpr,+ppx" uwtable {
; CHECK-LABEL: egpr:
; CHECK:         .seh_unwindversion 2
; CHECK-NEXT:    pushp %[[OUTER:r15|rbp]]
; CHECK-NOT:     %r3{{[01]}}
; CHECK:         popp %[[OUTER]]
; CHECK-NEXT:    .seh_endepilogue
; V3: error: unwind version 3 is not supported on 'x86_64-unknown-windows-itanium', which takes version 2
  call void asm sideeffect "", "~{rbx},~{r12},~{r13},~{r14},~{r15}"()
  call void @exti(i64 %a)
  %s = add i64 %a, %b
  ret i64 %s
}

!llvm.module.flags = !{!0}
!0 = !{i32 1, !"no-unwind-version", i32 3}

; RUN: llc -mtriple=x86_64-unknown-windows-itanium -O2 -verify-machineinstrs < %s | FileCheck %s
; RUN: llc -mtriple=x86_64-pc-windows-ntposix -O2 < %s | FileCheck %s
; RUN: llc -mtriple=x86_64-pc-windows-msvc -O2 < %s | FileCheck --check-prefix=HOISTED %s

; A call to a function in another image reads the address from the import
; address table. Loop-invariant code motion lifts that read out of the loop and
; leaves an indirect call behind, which costs an indirect branch on every
; iteration and leaves nothing at the call site naming the function. Folding
; the read back in restores the instruction the linker rewrites into a direct
; call when the function turns out to be defined in the image.

@gi = external dllimport global i32

declare dllimport void @f(i32)
declare void @sink(i32)

; The read stays, because whether it is wanted is the linker's to decide, and
; the call names the import again. Nothing is added to the call itself: the
; record is what says it can be turned back.

; CHECK-LABEL: loop:
; CHECK:       movq __imp_f(%rip), %[[REG:[a-z0-9]+]]
; CHECK:       callq *__imp_f(%rip)

; HOISTED-LABEL: loop:
; HOISTED:       movq __imp_f(%rip), %rbx
; HOISTED:       callq *%rbx
; HOISTED-NOT:   .impfuse


define void @loop(i32 %n) {
entry:
  %c = icmp sgt i32 %n, 0
  br i1 %c, label %body, label %exit
body:
  %i = phi i32 [ %inc, %body ], [ 0, %entry ]
  tail call void @f(i32 %i)
  %inc = add nuw nsw i32 %i, 1
  %done = icmp eq i32 %inc, %n
  br i1 %done, label %exit, label %body
exit:
  ret void
}

; A register read as the base of a memory operand is not an address a call can
; be given, so the read stays where it was hoisted to.

; CHECK-LABEL: data:
; CHECK:       movq __imp_gi(%rip), %[[REG:[a-z0-9]+]]
; CHECK:       movl (%[[REG]]),

define void @data(i32 %n) {
entry:
  %c = icmp sgt i32 %n, 0
  br i1 %c, label %body, label %exit
body:
  %i = phi i32 [ %inc, %body ], [ 0, %entry ]
  %v = load volatile i32, ptr @gi, align 4
  call void @sink(i32 %v)
  %inc = add nuw nsw i32 %i, 1
  %done = icmp eq i32 %inc, %n
  br i1 %done, label %exit, label %body
exit:
  ret void
}

; A use that is not a call goes on reading the register, and the call beside it
; is still folded: the read stays either way, so one does not hold the other
; back.

; CHECK-LABEL: mixed:
; CHECK:       movq __imp_f(%rip), %[[BOTH:[a-z0-9]+]]
; CHECK:       callq *__imp_f(%rip)

define void @mixed(i32 %n, ptr %out) {
entry:
  %c = icmp sgt i32 %n, 0
  br i1 %c, label %body, label %exit
body:
  %i = phi i32 [ %inc, %body ], [ 0, %entry ]
  tail call void @f(i32 %i)
  store volatile ptr @f, ptr %out, align 8
  %inc = add nuw nsw i32 %i, 1
  %done = icmp eq i32 %inc, %n
  br i1 %done, label %exit, label %body
exit:
  ret void
}

; The records come at the end of the file: the function each call is in, the
; pointer it reads, the register that holds it, and how far into the function
; the call is. The one call that was not folded has none.

; CHECK:       .section .impfuse$y
; CHECK-NEXT:  .symidx loop
; CHECK-NEXT:  .symidx __imp_f
; CHECK-NEXT:  .long
; CHECK-NEXT:  .long .Ltmp{{[0-9]+}}-loop
; CHECK-NEXT:  .symidx mixed
; CHECK-NEXT:  .symidx __imp_f
; CHECK-NEXT:  .long
; CHECK-NEXT:  .long .Ltmp{{[0-9]+}}-mixed

; And the read those calls were folded out of, when every one of its readers
; became such a call. The one in mixed has a reader that did not, so it has no
; record and stays whatever the linker makes of it.

; CHECK:       .section .impload$y
; CHECK-NEXT:  .symidx loop
; CHECK-NEXT:  .symidx __imp_f
; CHECK-NEXT:  .long .Ltmp{{[0-9]+}}-loop
; CHECK-NEXT:  .long
; CHECK-NEXT:  .long
; The last word says whether what could carry the read's bytes is a transfer
; of control, since seven more bytes in front of one can carry it across a
; thirty-two byte boundary.
; CHECK-NEXT:  .long
; CHECK-NOT:   .symidx

; HOISTED-NOT: .impfuse

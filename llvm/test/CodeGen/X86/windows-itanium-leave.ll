; DEFINE: %{llc} = llc -mtriple=x86_64-unknown-windows-itanium
; DEFINE: %{v1} = -x86-wineh-unwindv2-force-mode=0
; RUN: split-file %s %t
; RUN: %{llc} %{v1} < %t/a.ll | FileCheck %s --check-prefixes=ASM,ASM1
; RUN: %{llc} < %t/a.ll | FileCheck %s --check-prefixes=ASM,ASM2
; RUN: llc -mtriple=x86_64-pc-windows-ntposix < %t/a.ll | FileCheck %s --check-prefix=NTP
; RUN: %{llc} %{v1} -filetype=obj %t/a.ll -o %t.1.o
; RUN: llvm-readobj -r %t.1.o | FileCheck %s --check-prefix=OBJ1
; RUN: %{llc} -filetype=obj %t/a.ll -o %t.2.o
; RUN: llvm-readobj -r --unwind %t.2.o | FileCheck %s --check-prefix=OBJ2
; RUN: llc -mtriple=x86_64-pc-windows-msvc < %t/a.ll | FileCheck %s --check-prefix=MSVC \
; RUN:   --implicit-check-not=leave --implicit-check-not=large
; RUN: llc -mtriple=x86_64-pc-windows-msvc -x86-wineh-unwindv2-force-mode=2 < %t/a.ll \
; RUN:   | FileCheck %s --check-prefix=MSVC --implicit-check-not=leave \
; RUN:     --implicit-check-not=large
; RUN: %{llc} < %t/cleanup.ll | FileCheck %s --check-prefix=CL

; On Windows Itanium and NT-POSIX a function with a frame register pushes it
; after every other callee-saved register and sets it at its own saved value,
; or above the saved XMM registers, which the unwind data addresses from
; there, where what it addresses lies within an 8-bit displacement below it.
; Its epilogue is `leave`, then the other pops: no address has the frame half
; released. With nothing to release it pops the frame register instead, except
; before a jump that version 1 lets the linker delete only after `leave`.
; Version 2 unwind data describes the frame register's push as an allocation
; in the large form and a save, and names each epilogue from its first pop
; after `leave`. A jump that ends its function is BRANCH_TAIL where the linker
; may delete it: under version 1 after `leave` alone or the stack adjustment
; of a frame without a push, under version 2 after any epilogue, whose
; distances are relocations.

;--- a.ll
declare void @ext(ptr)
declare void @exti(i64)
declare double @extd(double)
declare i64 @tgt(i64)

; MSVC-LABEL: odd:
; MSVC:       pushq %rbp
; MSVC-NEXT:  .seh_pushreg %rbp
; MSVC-NEXT:  pushq %rsi
; MSVC:       popq %rsi
; MSVC-NEXT:  popq %rbp

define i64 @fp(i64 %a) "frame-pointer"="all" uwtable {
; ASM-LABEL: fp:
; ASM2-NEXT: .seh_proc fp
; ASM2:      .seh_unwindversion 2
; ASM:       pushq %rbp
; ASM1-NEXT: .seh_pushreg %rbp
; ASM2-NEXT: .seh_stackalloc 8, large
; ASM2-NEXT: .seh_savereg %rbp, 0
; ASM-NEXT:  movq %rsp, %rbp
; ASM-NEXT:  .seh_setframe %rbp, 0
; ASM-NEXT:  subq $64, %rsp
; ASM-NEXT:  .seh_stackalloc 64
; ASM-NEXT:  .seh_endprologue
; ASM:       .seh_startepilogue
; ASM-NEXT:  leave
; ASM2-NEXT: .seh_unwindv2start
; ASM-NEXT:  .seh_endepilogue
; ASM-NEXT:  jmp tgt
; NTP-LABEL: fp:
; NTP:       movq %rsp, %rbp
; NTP-NEXT:  .seh_setframe %rbp, 0
; NTP:       .seh_startepilogue
; NTP-NEXT:  leave
; NTP-NEXT:  .seh_unwindv2start
; NTP-NEXT:  .seh_endepilogue
; NTP-NEXT:  jmp tgt
  %b = alloca [24 x i8]
  call void @ext(ptr %b)
  %r = tail call i64 @tgt(i64 1)
  ret i64 %r
}

; An odd number of other pushes leaves the frame register 8 bytes off 16, so
; the XMM slot lies 8 bytes off 16 above where it is set; the unwind data
; then states its offset in bytes. Under version 1 the jump stays: past the
; pop, the unwinder would read the target for the epilogue.
define i64 @odd(i64 %a) "frame-pointer"="all" uwtable {
; ASM-LABEL: odd:
; ASM:       pushq %rsi
; ASM-NEXT:  .seh_pushreg %rsi
; ASM-NEXT:  pushq %rbp
; ASM1-NEXT: .seh_pushreg %rbp
; ASM2-NEXT: .seh_stackalloc 8, large
; ASM-NEXT:  subq $32, %rsp
; ASM-NEXT:  .seh_stackalloc 32
; ASM2-NEXT: .seh_savereg %rbp, 32
; ASM-NEXT:  leaq 32(%rsp), %rbp
; ASM-NEXT:  .seh_setframe %rbp, 32
; ASM-NEXT:  subq $56, %rsp
; ASM-NEXT:  .seh_stackalloc 56
; ASM-NEXT:  movaps %xmm6, -24(%rbp)
; ASM-NEXT:  .seh_savexmm %xmm6, 8
; ASM-NEXT:  .seh_endprologue
; ASM:       movaps -24(%rbp), %xmm6
; ASM-NEXT:  .seh_startepilogue
; ASM-NEXT:  leave
; ASM2-NEXT: .seh_unwindv2start
; ASM-NEXT:  popq %rsi
; ASM-NEXT:  .seh_endepilogue
; ASM-NEXT:  jmp tgt
  %b = alloca [24 x i8]
  %d = sitofp i64 %a to double
  call void @ext(ptr %b)
  %e = call double @extd(double %d)
  call void @exti(i64 %a)
  %r = tail call i64 @tgt(i64 %a)
  ret i64 %r
}

; With nothing else below the XMM slot the allocation grows to reach the frame
; register's offset, by a multiple of 16: the stack stays aligned at the call.
define i64 @odd_dyn(i64 %a, i64 %n) uwtable {
; ASM-LABEL: odd_dyn:
; ASM:       subq $32, %rsp
; ASM:       leaq 32(%rsp), %rbp
; ASM-NEXT:  .seh_setframe %rbp, 32
; ASM-NEXT:  pushq %rax
; ASM-NEXT:  .seh_stackalloc 8
; ASM-NEXT:  movaps %xmm6, -24(%rbp)
; ASM-NEXT:  .seh_savexmm %xmm6, 8
; ASM-NEXT:  .seh_endprologue
  %b = alloca i8, i64 %n
  call void asm sideeffect "", "~{xmm6}"()
  call void @ext(ptr %b)
  ret i64 %a
}

; One allocation reaches the frame register's offset where it can.
define i64 @even(i64 %a, i64 %c) "frame-pointer"="all" uwtable {
; ASM-LABEL: even:
; ASM:       pushq %rsi
; ASM:       pushq %rdi
; ASM:       pushq %rbp
; ASM:       subq $80, %rsp
; ASM-NEXT:  .seh_stackalloc 80
; ASM2-NEXT: .seh_savereg %rbp, 80
; ASM-NEXT:  leaq 80(%rsp), %rbp
; ASM-NEXT:  .seh_setframe %rbp, 80
; ASM-NEXT:  movaps %xmm6, -16(%rbp)
; ASM-NEXT:  .seh_savexmm %xmm6, 64
; ASM:       .seh_startepilogue
; ASM-NEXT:  leave
; ASM2-NEXT: .seh_unwindv2start
; ASM-NEXT:  popq %rdi
; ASM-NEXT:  popq %rsi
; ASM-NEXT:  .seh_endepilogue
; ASM-NEXT:  retq
  %b = alloca [24 x i8]
  %d = sitofp i64 %a to double
  call void @ext(ptr %b)
  %e = call double @extd(double %d)
  call void @exti(i64 %c)
  ret i64 %a
}

define i64 @empty(i64 %a) "frame-pointer"="all" uwtable {
; ASM-LABEL: empty:
; ASM:       pushq %rbp
; ASM-NEXT:  .seh_pushreg %rbp
; ASM-NEXT:  movq %rsp, %rbp
; ASM-NEXT:  .seh_setframe %rbp, 0
; ASM-NEXT:  .seh_endprologue
; ASM:       .seh_startepilogue
; ASM1-NEXT: leave
; ASM2-NEXT: .seh_unwindv2start
; ASM2-NEXT: popq %rbp
; ASM-NEXT:  .seh_endepilogue
; ASM-NEXT:  jmp tgt
  %r = tail call i64 @tgt(i64 1)
  ret i64 %r
}

define i64 @emptyret(i64 %a) "frame-pointer"="all" uwtable {
; ASM-LABEL: emptyret:
; ASM:       .seh_startepilogue
; ASM2-NEXT: .seh_unwindv2start
; ASM-NEXT:  popq %rbp
; ASM-NEXT:  .seh_endepilogue
; ASM-NEXT:  retq
  ret i64 %a
}

define i64 @dyn(i64 %a) uwtable {
; ASM-LABEL: dyn:
; ASM:       pushq %rsi
; ASM-NEXT:  .seh_pushreg %rsi
; ASM-NEXT:  pushq %rbp
; ASM:       movq %rsp, %rbp
; ASM:       .seh_startepilogue
; ASM-NEXT:  leave
; ASM2-NEXT: .seh_unwindv2start
; ASM-NEXT:  popq %rsi
; ASM-NEXT:  .seh_endepilogue
; ASM-NEXT:  jmp tgt
  %b = alloca i8, i64 %a
  call void @ext(ptr %b)
  %r = tail call i64 @tgt(i64 %a)
  ret i64 %r
}

; A realigned frame addresses its locals from the stack pointer.
define i64 @realign(i64 %a) uwtable {
; ASM-LABEL: realign:
; ASM:       movq %rsp, %rbp
; ASM-NEXT:  .seh_setframe %rbp, 0
; ASM-NEXT:  subq $304, %rsp
; ASM-NEXT:  .seh_stackalloc 304
; ASM-NEXT:  .seh_endprologue
; ASM-NEXT:  andq $-32, %rsp
; ASM:       .seh_startepilogue
; ASM-NEXT:  leave
; ASM2-NEXT: .seh_unwindv2start
; ASM-NEXT:  .seh_endepilogue
; ASM-NEXT:  retq
  %b = alloca [256 x i8], align 32
  call void @ext(ptr %b)
  ret i64 1
}

; Where the frame reaches further than 128 bytes below the saved frame
; register, a frame register set toward its middle reaches more of it.
define i64 @big(i64 %a) "frame-pointer"="all" uwtable {
; ASM-LABEL: big:
; ASM:       pushq %rbp
; ASM-NEXT:  .seh_pushreg %rbp
; ASM:       leaq 128(%rsp), %rbp
; ASM:       .seh_startepilogue
; ASM-NEXT:  addq $288, %rsp
; ASM2-NEXT: .seh_unwindv2start
; ASM-NEXT:  popq %rbp
; ASM-NEXT:  .seh_endepilogue
; ASM-NEXT:  jmp tgt
  %b = alloca [256 x i8]
  call void @ext(ptr %b)
  %r = tail call i64 @tgt(i64 1)
  ret i64 %r
}

define i64 @nopush(i64 %a) uwtable {
; ASM-LABEL: nopush:
; ASM:       .seh_startepilogue
; ASM-NEXT:  addq $56, %rsp
; ASM2-NEXT: .seh_unwindv2start
; ASM-NEXT:  .seh_endepilogue
; ASM-NEXT:  jmp tgt
  %b = alloca [24 x i8]
  call void @ext(ptr %b)
  %r = tail call i64 @tgt(i64 1)
  ret i64 %r
}

define i64 @push(i64 %a) uwtable {
; ASM-LABEL: push:
; ASM:       .seh_startepilogue
; ASM-NEXT:  addq $64, %rsp
; ASM2-NEXT: .seh_unwindv2start
; ASM-NEXT:  popq %rsi
; ASM-NEXT:  .seh_endepilogue
; ASM-NEXT:  jmp tgt
  %b = alloca [24 x i8]
  call void @ext(ptr %b)
  %r = tail call i64 @tgt(i64 %a)
  ret i64 %r
}

; The jumps of fp, odd, empty, dyn, big, nopush and push.
; OBJ1:      Section ({{[0-9]+}}) .text {
; OBJ1:        IMAGE_REL_LLVM_BRANCH_TAIL tgt
; OBJ1:        IMAGE_REL_LLVM_JUMP32 tgt
; OBJ1:        IMAGE_REL_LLVM_BRANCH_TAIL tgt
; OBJ1:        IMAGE_REL_LLVM_JUMP32 tgt
; OBJ1:        IMAGE_REL_LLVM_JUMP32 tgt
; OBJ1:        IMAGE_REL_LLVM_BRANCH_TAIL tgt
; OBJ1:        IMAGE_REL_LLVM_JUMP32 tgt
; OBJ1-NEXT: }

; OBJ2:      Section ({{[0-9]+}}) .text {
; OBJ2-COUNT-7: IMAGE_REL_LLVM_BRANCH_TAIL tgt
; OBJ2-NEXT: }
; The distances of fp, odd, empty, dyn, big, nopush and push; even,
; emptyret and realign end in a return at the function's end.
; OBJ2:      Section ({{[0-9]+}}) .xdata {
; OBJ2-COUNT-7: IMAGE_REL_LLVM_SSPAN_EPILOGV2
; OBJ2-NEXT:   IMAGE_REL_AMD64_PAIR
; OBJ2-NEXT: }
; OBJ2:      StartAddress: fp
; OBJ2:        EPILOG atend=no, length=0x1
; OBJ2-NEXT:   EPILOG offset=0x5
; OBJ2:      StartAddress: odd
; OBJ2:        EPILOG atend=no, length=0x2
; OBJ2-NEXT:   EPILOG offset=0x6
; OBJ2:      StartAddress: even
; OBJ2:        EPILOG atend=yes, length=0x3
; OBJ2-NEXT:   EPILOG padding

;--- cleanup.ll
declare void @ext(ptr)
declare i32 @__gxx_personality_seh0(...)

; Funclets that find a realigned frame from the establisher frame need it to be
; the stack pointer the prologue leaves: the frame register is set after the
; allocation.
define void @cleanup(i64 %a) uwtable personality ptr @__gxx_personality_seh0 {
; CL-LABEL:  cleanup:
; CL:        pushq %rbp
; CL-NEXT:   .seh_pushreg %rbp
; CL-NEXT:   pushq %rbx
; CL:        leaq 64(%rsp), %rbp
; CL-NEXT:   .seh_setframe %rbp, 64
; CL:        .seh_startepilogue
; CL-NEXT:   leaq 8(%rbp), %rsp
  %b = alloca [8 x i8], align 32
  invoke void @ext(ptr %b) to label %cont unwind label %ehc
ehc:
  %p = cleanuppad within none []
  call void @ext(ptr %b) [ "funclet"(token %p) ]
  cleanupret from %p unwind to caller
cont:
  ret void
}

; RUN: llc < %s -mtriple=x86_64-unknown-windows-itanium | \
; RUN:   FileCheck %s --check-prefixes=CHECK,FUSED,WI
; RUN: llc < %s -mtriple=x86_64-pc-windows-ntposix | \
; RUN:   FileCheck %s --check-prefixes=CHECK,FUSED,NT
; RUN: llc < %s -mtriple=x86_64-pc-windows-msvc | \
; RUN:   FileCheck %s --check-prefixes=CHECK,PLAIN --implicit-check-not=FUSED \
; RUN:   --implicit-check-not=int3
; RUN: llc < %s -mtriple=x86_64-unknown-windows-itanium -filetype=obj -o %t.o
; RUN: llvm-readobj -r --coff-link-records %t.o | \
; RUN:   FileCheck %s --check-prefix=OBJ
; RUN: llvm-objdump -d %t.o | FileCheck %s --check-prefix=DIS
; RUN: %if aarch64-registered-target %{ llc < %s -mtriple=aarch64-unknown-windows-itanium | FileCheck %s --check-prefixes=A64,A64F %}
; RUN: %if aarch64-registered-target %{ llc < %s -mtriple=aarch64-pc-windows-ntposix | FileCheck %s --check-prefixes=A64,A64F %}
; RUN: %if aarch64-registered-target %{ llc < %s -mtriple=aarch64-pc-windows-msvc | \
; RUN:   FileCheck %s --check-prefixes=A64,A64P --implicit-check-not=fused --implicit-check-not=FUSED %}
; RUN: %if aarch64-registered-target %{ llc < %s -mtriple=aarch64-unknown-windows-itanium -filetype=obj | \
; RUN:   llvm-readobj -r --coff-link-records - | FileCheck %s --check-prefix=A64OBJ %}

; On Windows Itanium and NT-POSIX a load of an import pointer whose register
; only calls and tail calls through it read, each reached by that load alone,
; is a fused read, each of the calls a fused call and each of the tail calls a
; fused jump that names the pointer. On x86-64 a fused call has the CS
; overrides that make it six bytes, and a fused jump the int3 that make it
; seven. Any other instruction is that of every other Windows triple.

declare dllimport void @imported()
declare dllimport void @taking(ptr)

; The load in a loop's preheader reaches the call in the loop alone. A call
; through one of the registers r8 to r15 has a prefix of its own.
define void @loop(i32 %n) {
; CHECK-LABEL: loop:
; FUSED:         movq __imp_imported@FUSED(%rip), [[REG:%r[a-z0-9]+]]
; PLAIN:         movq __imp_imported(%rip), [[REG:%r[a-z0-9]+]]
; CHECK:       .LBB0_1:
; FUSED:       [[SITE:.Ltmp[0-9]+]]:
; FUSED-NEXT:    .reloc [[SITE]], IMAGE_REL_LLVM_FUSED_CALL, __imp_imported
; FUSED-COUNT-3: cs
; WI-NEXT:       cs
; WI-NEXT:       callq *%rdi
; NT-NEXT:       callq *%r14
; PLAIN:         callq *[[REG]]
; A64:         adrp x20, __imp_imported
; A64F:          ldr x20, [x20, :fused_lo12:__imp_imported]
; A64P:          ldr x20, [x20, :lo12:__imp_imported]
; A64:         // %body
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

; Two calls read one load.
define void @twice() {
; CHECK-LABEL: twice:
; FUSED:         movq __imp_imported@FUSED(%rip), [[REG:%r[a-z0-9]+]]
; FUSED-NEXT:  [[SITE:.Ltmp[0-9]+]]:
; FUSED-NEXT:    .reloc [[SITE]], IMAGE_REL_LLVM_FUSED_CALL, __imp_imported
; FUSED-COUNT-4: cs
; PLAIN:         movq __imp_imported(%rip), [[REG:%r[a-z0-9]+]]
; CHECK-NEXT:    callq *[[REG]]
; FUSED-NEXT:  [[SITE:.Ltmp[0-9]+]]:
; FUSED-NEXT:    .reloc [[SITE]], IMAGE_REL_LLVM_FUSED_CALL, __imp_imported
; FUSED-COUNT-4: cs
; CHECK-NEXT:    callq *[[REG]]
; A64-LABEL: twice:
; A64F:          ldr x19, [x19, :fused_lo12:__imp_imported]
; A64F-NEXT:   [[SITE:.Ltmp[0-9]+]]:
; A64F-NEXT:     .reloc [[SITE]], IMAGE_REL_LLVM_FUSED_CALL, __imp_imported
; A64F-NEXT:     blr x19
; A64F-NEXT:   [[SITE:.Ltmp[0-9]+]]:
; A64F-NEXT:     .reloc [[SITE]], IMAGE_REL_LLVM_FUSED_CALL, __imp_imported
; A64F-NEXT:     blr x19
  call void @imported()
  call void @imported()
  ret void
}

; A tail call through the register is a fused jump.
define void @tail() optnone noinline {
; CHECK-LABEL: tail:
; FUSED:         movq __imp_imported@FUSED(%rip), %rax
; FUSED-NEXT:  [[SITE:.Ltmp[0-9]+]]:
; FUSED-NEXT:    .reloc [[SITE]], IMAGE_REL_LLVM_FUSED_JUMP, __imp_imported
; PLAIN:         movq __imp_imported(%rip), %rax
; CHECK-NEXT:    rex64 jmpq *%rax
; FUSED-COUNT-4: int3
; A64-LABEL: tail:
; A64F:          ldr x0, [x0, :fused_lo12:__imp_imported]
; A64F-NEXT:   [[SITE:.Ltmp[0-9]+]]:
; A64F-NEXT:     .reloc [[SITE]], IMAGE_REL_LLVM_FUSED_JUMP, __imp_imported
; A64F-NEXT:     br x0
  musttail call void @imported()
  ret void
}

; A store reads the register, so the load stays a load and each call a call.
define void @stored(ptr %p) {
; CHECK-LABEL: stored:
; CHECK:         movq __imp_imported(%rip), [[REG:%r[a-z0-9]+]]
; CHECK-NEXT:    movq [[REG]], (%r{{[a-z]+}})
; CHECK-NEXT:    callq *[[REG]]
; CHECK-NEXT:    callq *[[REG]]
; A64-LABEL: stored:
; A64:           ldr x19, [x19, :lo12:__imp_imported]
; A64-NEXT:      str x19, [x0]
  store ptr @imported, ptr %p
  call void @imported()
  call void @imported()
  ret void
}

; So does a comparison after the calls.
define i1 @compared(ptr %p) {
; CHECK-LABEL: compared:
; CHECK:         movq __imp_imported(%rip), [[REG:%r[a-z0-9]+]]
; CHECK-NEXT:    callq *[[REG]]
; CHECK-NEXT:    callq *[[REG]]
; CHECK-NEXT:    cmpq [[REG]], %r{{[a-z]+}}
; A64-LABEL: compared:
; A64:           ldr x20, [x20, :lo12:__imp_imported]
; A64-NEXT:      blr x20
; A64-NEXT:      blr x20
; A64-NEXT:      cmp x19, x20
  call void @imported()
  call void @imported()
  %c = icmp eq ptr %p, @imported
  ret i1 %c
}

; The calls are also reached with what the function was given in the register.
define void @merged(i1 %c, ptr %p) {
; CHECK-LABEL: merged:
; CHECK:         movq __imp_imported(%rip), [[REG:%r[a-z0-9]+]]
; CHECK:         movq %r{{[a-z]+}}, [[REG]]
; CHECK:         callq *[[REG]]
; CHECK-NEXT:    callq *[[REG]]
; A64-LABEL: merged:
; A64:           ldr x19, [x19, :lo12:__imp_imported]
; A64:           blr x19
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

; A copy to the register of the tail call reads the register of the call. On
; AArch64 the epilogue lies between the load and the tail call, and writes
; another register.
define void @call_then_tail() {
; CHECK-LABEL: call_then_tail:
; CHECK:         movq __imp_imported(%rip), [[REG:%r[a-z0-9]+]]
; CHECK-NEXT:    callq *[[REG]]
; CHECK-NEXT:    movq [[REG]], %rax
; CHECK:         rex64 jmpq *%rax
; A64-LABEL: call_then_tail:
; A64:           blr x0
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

; The field of a fused read holds twice the length of the instruction after
; it plus one where that transfers control, or zero where padding follows.
; DIS-LABEL: <loop>:
; DIS:         48 8b 3d 00 00 00 00   movq
; DIS:         2e 2e 2e 2e ff d7      callq *%rdi
; DIS-LABEL: <twice>:
; DIS:         48 8b 35 0d 00 00 00   movq
; DIS-NEXT:    2e 2e 2e 2e ff d6      callq *%rsi
; DIS-NEXT:    2e 2e 2e 2e ff d6      callq *%rsi
; DIS-LABEL: <tail>:
; DIS:         48 8b 05 07 00 00 00   movq
; DIS-NEXT:    48 ff e0               jmpq *%rax
; DIS-NEXT:    cc                     int3
; DIS-NEXT:    cc                     int3
; DIS-NEXT:    cc                     int3
; DIS-NEXT:    cc                     int3

; OBJ:      Section ({{[0-9]+}}) .text {
; OBJ-NEXT:   0xB IMAGE_REL_LLVM_FUSED_READ __imp_imported
; OBJ-NEXT:   0x10 IMAGE_REL_LLVM_FUSED_CALL __imp_imported
; OBJ-NEXT:   IMAGE_REL_LLVM_ALIGN
; OBJ-NEXT:   0x38 IMAGE_REL_LLVM_FUSED_READ __imp_imported
; OBJ-NEXT:   0x3C IMAGE_REL_LLVM_FUSED_CALL __imp_imported
; OBJ-NEXT:   0x42 IMAGE_REL_LLVM_FUSED_CALL __imp_imported
; OBJ-NEXT:   IMAGE_REL_LLVM_ALIGN
; OBJ-NEXT:   0x53 IMAGE_REL_LLVM_FUSED_READ __imp_imported
; OBJ-NEXT:   0x57 IMAGE_REL_LLVM_FUSED_JUMP __imp_imported
; OBJ-NEXT:   IMAGE_REL_LLVM_ALIGN
; OBJ-NEXT:   IMAGE_REL_LLVM_MEM_LOAD __imp_imported
; OBJ-NOT:    FUSED
; OBJ:      Flags: 0x8

; The linker may resize the code, so a branch across an adrp names a label.
; A function's alignment of 4 bytes has no ALIGN relocation and leaves the
; section listed.
; A64OBJ:      0x8 IMAGE_REL_ARM64_PAGEBASE_REL21 __imp_imported
; A64OBJ-NEXT: 0x10 IMAGE_REL_LLVM_FUSED_READ __imp_imported
; A64OBJ-NEXT: 0x14 IMAGE_REL_LLVM_FUSED_CALL __imp_imported
; A64OBJ-NOT:  ALIGN
; A64OBJ:      IMAGE_REL_ARM64_BRANCH14 $L{{[0-9a-f]+}}
; A64OBJ-NOT:  ALIGN
; A64OBJ:      Kind: Sections
; A64OBJ-NEXT: ResizableSection: .text

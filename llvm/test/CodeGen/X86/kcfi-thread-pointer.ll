; RUN: llc -mtriple=x86_64-pc-windows-ntposix < %s | FileCheck %s
; RUN: llc -mtriple=x86_64-pc-windows-ntposix -filetype=obj < %s -o %t.o
; RUN: llvm-readobj -r --sections --section-data %t.o | FileCheck %s --check-prefix=OBJ
; RUN: sed 's/^!llvm.module.flags = !{!0, !1}/!llvm.module.flags = !{!0, !1, !4}/' %s \
; RUN:   | llc -mtriple=x86_64-pc-windows-ntposix | FileCheck %s --check-prefix=CFG

;; The word is the entry's distance from the function, a REL32 of its symbol
;; from a field 16 bytes before the function.
; OBJ:      0100: F4FFFFFF 0F1F8210 000000B8 4E61BC80
; OBJ:      0160: {{.*}} 07000000
; OBJ-NEXT: 0170: F4FFFFFF 0F1F8310 000000B8 4E61BC80
; OBJ:      0x100 IMAGE_REL_AMD64_REL32 reader$tp
; OBJ:      0x170 IMAGE_REL_AMD64_REL32 vreader$tp

declare ptr @llvm.thread.pointer.p0()

define void @reader(ptr %p) "thread-pointer-argument" !kcfi_type !2 {
  %tp = call ptr @llvm.thread.pointer.p0()
  store ptr %tp, ptr %p
  ret void
}

define void @vreader(ptr %p) "thread-pointer-argument" !kcfi_type !2 !kcfi_vfn_type !3 {
  %tp = call ptr @llvm.thread.pointer.p0()
  store ptr %tp, ptr %p
  ret void
}

;; A function that holds the thread pointer passes it on a checked call
;; through a pointer. The check accepts the type with or without its tag,
;; which doubling moves into the carry, and goes to the target plus its
;; offset where the target is tagged and its prefix holds one: in place where
;; the call clobbers the target's register, through R11 where it keeps it.
;; Under Control Flow Guard checks, which accept no thread-pointer entry, the
;; call passes none.

; CHECK-LABEL: holder$tp:
; CHECK:         movl $4282621618, %r10d
; CHECK-NEXT:    addl -4(%r15), %r10d
; CHECK-NEXT:    addl %r10d, %r10d
; CHECK-NEXT:    je [[PASS:.Ltmp[0-9]+]]
; CHECK-NEXT:  [[TRAP:.Ltmp[0-9]+]]:
; CHECK-NEXT:    ud2
; CHECK-NEXT:  [[PASS]]:
; CHECK-NEXT:    movq %r15, %r11
; CHECK-NEXT:    jae [[CALL:.Ltmp[0-9]+]]
; CHECK-NEXT:    testb $2, -10(%r15)
; CHECK-NEXT:    je [[TRAP]]
; CHECK-NEXT:    movslq -16(%r15), %r10
; CHECK-NEXT:    addq %r10, %r11
; CHECK-NEXT:  [[CALL]]:
; CHECK-NEXT:    callq *%r11
; CFG-LABEL:   holder$tp:
; CFG-NOT:       addl %r10d, %r10d
; CFG:           callq *%rcx
; CFG-NOT:       addl %r10d, %r10d
; CFG:         .Lfunc_end
define void @holder(ptr %f, ptr %p) "thread-pointer-argument" !kcfi_type !2 {
  %tp = call ptr @llvm.thread.pointer.p0()
  store ptr %tp, ptr %p
  call void %f(ptr %p) [ "kcfi"(i32 12345678) ]
  call void %f(ptr %p) [ "kcfi"(i32 12345678) ]
  ret void
}

; CHECK-LABEL: tail$tp:
; CHECK:         addl %r10d, %r10d
; CHECK-NEXT:    je [[PASS:.Ltmp[0-9]+]]
; CHECK-NEXT:  [[TRAP:.Ltmp[0-9]+]]:
; CHECK-NEXT:    ud2
; CHECK-NEXT:  [[PASS]]:
; CHECK-NEXT:    jae [[CALL:.Ltmp[0-9]+]]
; CHECK-NEXT:    testb $2, -10(%rax)
; CHECK-NEXT:    je [[TRAP]]
; CHECK-NEXT:    movslq -16(%rax), %r10
; CHECK-NEXT:    addq %r10, %rax
; CHECK-NEXT:  [[CALL]]:
; CHECK-NEXT:    rex64 jmpq *%rax # TAILCALL
define void @tail(ptr %f, ptr %p) "thread-pointer-argument" !kcfi_type !2 {
  %tp = call ptr @llvm.thread.pointer.p0()
  store ptr %tp, ptr %p
  tail call void %f(ptr %p) [ "kcfi"(i32 12345678) ]
  ret void
}

;; The type prefix of a plain entry with a body of its own, emitted after the
;; bodies, holds the offset of its thread-pointer entry in the word before the
;; marker, which the nopl states by its base register, RDX, or RBX after a
;; second type, and sets bit 31 of the type. The link fact states the words.

; CHECK:      {{^}}__cfi_reader:
; CHECK-NEXT: [[W:.Ltmp[0-9]+]]:
; CHECK-NEXT:   .long reader$tp-([[W]]+16)
; CHECK-NEXT:   {disp32} nopl 16(%rdx)
; CHECK-NEXT:   movl $2159829326, %eax # imm = 0x80BC614E
; CHECK-NEXT: {{^}}reader:
; CHECK:      {{^}}__cfi_vreader:
; CHECK-NEXT:   .long 7
; CHECK-NEXT: [[W:.Ltmp[0-9]+]]:
; CHECK-NEXT:   .long vreader$tp-([[W]]+16)
; CHECK-NEXT:   {disp32} nopl 16(%rbx)
; CHECK-NEXT:   movl $2159829326, %eax
; CHECK-NEXT: {{^}}vreader:
; CHECK:      .link_fact reader$tp, 0, 0
; CHECK:      .link_fact vreader$tp, 0, 1
; CHECK:      .link_fact reader, 0, 2
; CHECK:      .link_fact vreader, 0, 3

!llvm.module.flags = !{!0, !1}
!0 = !{i32 4, !"kcfi", i32 1}
!1 = !{i32 1, !"function-type-prefix", i32 16}
!2 = !{i32 12345678}
!3 = !{i32 7}
!4 = !{i32 2, !"cfguard", i32 2}

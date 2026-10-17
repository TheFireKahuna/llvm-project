; RUN: llc -mtriple=x86_64-unknown-windows-itanium < %s | FileCheck %s
; RUN: llc -mtriple=x86_64-unknown-windows-itanium < %s | \
; RUN:   FileCheck %s --check-prefix=DISP

;; A virtual call whose vtable pointer load carries vtable_guard metadata is
;; routed, by the CFGuard pass, through the class's guard thunk with the vtable
;; pointer as the dispatch target. The thunk takes the pointer in RAX, jumps
;; through the slot when the pointer is in the class's run [start, end) and a
;; multiple of the stride the mask word holds, and otherwise loads the slot and
;; continues into the type's KCFI dispatch thunk. It reads the run through
;; addresses and the mask word, never an absolute immediate.

; CHECK-LABEL: call:
; CHECK:         movq (%rcx), %rax
; CHECK:         callq __llvm_vcall_1A_8

; CHECK:       .section .text,"xr",discard,__llvm_vcall_1A_8
; CHECK:       __llvm_vcall_1A_8:
; CHECK-NEXT:    leaq __llvm_vtables$1A(%rip), %r10
; CHECK-NEXT:    cmpq %r10, %rax
; CHECK-NEXT:    jb [[MISS:\.L.*]]
; CHECK-NEXT:    movq %rax, %r11
; CHECK-NEXT:    subq %r10, %r11
; CHECK-NEXT:    leaq __llvm_vtables_end$1A(%rip), %r10
; CHECK-NEXT:    cmpq %r10, %rax
; CHECK-NEXT:    jae [[MISS]]
; CHECK-NEXT:    testl %r11d, __llvm_vtables_mask$1A(%rip)
; CHECK-NEXT:    jne [[MISS]]
; CHECK-NEXT:    jmpq *8(%rax)
; CHECK-NEXT:  [[MISS]]:
; CHECK-NEXT:    movq 8(%rax), %rax
; CHECK-NEXT:    jmp __llvm_kcfi_dispatch_0001e240

;; A class whose run another image may own too tests the run of the image
;; that owns it, from the start, end and start plus mask that its three words
;; hold, before the slow path.
; CHECK:       __llvm_vcall_1B_8:
; CHECK:         testl %r11d, __llvm_vtables_mask$1B(%rip)
; CHECK-NEXT:    jne [[OWNER:\.L.*]]
; CHECK-NEXT:    jmpq *8(%rax)
; CHECK-NEXT:  [[OWNER]]:
; CHECK-NEXT:    leaq __llvm_vtables_owner$1B(%rip), %r10
; CHECK-NEXT:    cmpq 8(%r10), %rax
; CHECK-NEXT:    jae [[MISS2:\.L.*]]
; CHECK-NEXT:    movq %rax, %r11
; CHECK-NEXT:    subq (%r10), %r11
; CHECK-NEXT:    jb [[MISS2]]
; CHECK-NEXT:    movq 16(%r10), %r10
; CHECK-NEXT:    subq %rax, %r10
; CHECK-NEXT:    addq %r11, %r10
; CHECK-NEXT:    testq %r10, %r11
; CHECK-NEXT:    jne [[MISS2]]
; CHECK-NEXT:    jmpq *8(%rax)
; CHECK-NEXT:  [[MISS2]]:
; CHECK-NEXT:    movq 8(%rax), %rax
; CHECK-NEXT:    jmp __llvm_kcfi_dispatch_0001e240

;; The dispatch thunk the miss path continues into is emitted even though only
;; the guard thunk's body reaches it (0x1e240 is the KCFI type).
; DISP: __llvm_kcfi_dispatch_0001e240:

define void @call(ptr %a) !kcfi_type !3 {
entry:
  %vtable = load ptr, ptr %a
  %slot = getelementptr inbounds i8, ptr %vtable, i64 8
  %f = load ptr, ptr %slot, !vtable_guard !4
  call void %f(ptr %a) [ "kcfi"(i32 123456) ]
  ret void
}

define void @owned(ptr %a) !kcfi_type !3 {
entry:
  %vtable = load ptr, ptr %a
  %slot = getelementptr inbounds i8, ptr %vtable, i64 8
  %f = load ptr, ptr %slot, !vtable_guard !5
  call void %f(ptr %a) [ "kcfi"(i32 123456) ]
  ret void
}

!llvm.module.flags = !{!0, !1, !2}
!0 = !{i32 4, !"kcfi", i32 1}
!1 = !{i32 4, !"function-type-prefix", i32 -559038737}
!2 = !{i32 1, !"MaxTLSAlign", i32 65536}
!3 = !{i32 123456}
!4 = !{!"1A", i64 8}
!5 = !{!"1B", i64 8, i1 true}

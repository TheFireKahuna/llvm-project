; REQUIRES: aarch64-registered-target
; RUN: llc -mtriple=x86_64-windows-itanium < %s | FileCheck %s
; RUN: llc -mtriple=aarch64-windows-itanium < %s | FileCheck %s --check-prefix=A64

;; A thread-local variable that may be another image's is reached through its
;; record, whose address is the import x$tls: the address of that image's TLS
;; index, then the offsets of the variable and of its guard in the image's
;; block of the thread. Clang emits this for a known import. The loads of the
;; record's words are marked as accesses through its pointer, and each
;; instruction that reads a word as a fused index, with a fact that states the
;; word and, for an offset, whether its register is the first of the two that
;; the instruction adds. A load that fused indexes alone read states so, and a
;; linker that finds the record in its image drops it.

@"x$tls" = external dllimport local_unnamed_addr constant { ptr, i32, i32 }

define i32 @constant() nounwind {
  %p = load ptr, ptr @"x$tls", align 8, !invariant.load !0, !dereferenceable !1, !align !1, !noundef !0
  %i = load i32, ptr %p, align 4, !invariant.load !0
  %a = tail call ptr @llvm.thread.pointer.p0()
  %ix = zext i32 %i to i64
  %s = getelementptr inbounds ptr, ptr %a, i64 %ix
  %b = load ptr, ptr %s, align 8
  %o = load i32, ptr getelementptr inbounds nuw (i8, ptr @"x$tls", i64 8), align 8, !invariant.load !0
  %ox = zext i32 %o to i64
  %v = getelementptr inbounds i8, ptr %b, i64 %ox
  %r = load i32, ptr %v, align 4
  ret i32 %r
}

define i32 @dynamic() nounwind {
  %p = load ptr, ptr @"x$tls", align 8, !invariant.load !0, !dereferenceable !1, !align !1, !noundef !0
  %i = load i32, ptr %p, align 4, !invariant.load !0
  %a = tail call ptr @llvm.thread.pointer.p0()
  %ix = zext i32 %i to i64
  %s = getelementptr inbounds ptr, ptr %a, i64 %ix
  %b = load ptr, ptr %s, align 8
  %o = load i32, ptr getelementptr inbounds nuw (i8, ptr @"x$tls", i64 8), align 8, !invariant.load !0
  %go = load i32, ptr getelementptr inbounds nuw (i8, ptr @"x$tls", i64 12), align 4, !invariant.load !0
  %gx = zext i32 %go to i64
  %g = getelementptr inbounds i8, ptr %b, i64 %gx
  %byte = load i8, ptr %g, align 1
  %unset = icmp eq i8 %byte, 0
  br i1 %unset, label %init, label %done, !prof !2

init:
  tail call void @_ZTH1x()
  br label %done

done:
  %ox = zext i32 %o to i64
  %v = getelementptr inbounds i8, ptr %b, i64 %ox
  %r = load i32, ptr %v, align 4
  ret i32 %r
}

define ptr @address() nounwind {
  %p = load ptr, ptr @"x$tls", align 8, !invariant.load !0, !dereferenceable !1, !align !1, !noundef !0
  %i = load i32, ptr %p, align 4, !invariant.load !0
  %a = tail call ptr @llvm.thread.pointer.p0()
  %ix = zext i32 %i to i64
  %s = getelementptr inbounds ptr, ptr %a, i64 %ix
  %b = load ptr, ptr %s, align 8
  %o = load i32, ptr getelementptr inbounds nuw (i8, ptr @"x$tls", i64 8), align 8, !invariant.load !0
  %ox = zext i32 %o to i64
  %v = getelementptr inbounds i8, ptr %b, i64 %ox
  ret ptr %v
}

define ptr @held(ptr %out) nounwind {
  %p = load ptr, ptr @"x$tls", align 8, !invariant.load !0, !dereferenceable !1, !align !1, !noundef !0
  %i = load i32, ptr %p, align 4, !invariant.load !0
  %a = tail call ptr @llvm.thread.pointer.p0()
  %ix = zext i32 %i to i64
  %s = getelementptr inbounds ptr, ptr %a, i64 %ix
  %b = load ptr, ptr %s, align 8
  %o = load i32, ptr getelementptr inbounds nuw (i8, ptr @"x$tls", i64 8), align 8, !invariant.load !0
  %ox = zext i32 %o to i64
  %v = getelementptr inbounds i8, ptr %b, i64 %ox
  store ptr %b, ptr %out
  ret ptr %v
}

define i32 @sunk() nounwind {
  %p = load ptr, ptr @"x$tls", align 8, !invariant.load !0, !dereferenceable !1, !align !1, !noundef !0
  %i = load i32, ptr %p, align 4, !invariant.load !0
  %a = tail call ptr @llvm.thread.pointer.p0()
  %ix = zext i32 %i to i64
  %s = getelementptr inbounds ptr, ptr %a, i64 %ix
  %b = load ptr, ptr %s, align 8
  %go = load i32, ptr getelementptr inbounds nuw (i8, ptr @"x$tls", i64 12), align 4, !invariant.load !0
  %gx = zext i32 %go to i64
  %g = getelementptr inbounds i8, ptr %b, i64 %gx
  %byte = load i8, ptr %g, align 1
  %unset = icmp eq i8 %byte, 0
  br i1 %unset, label %init, label %done, !prof !2

init:
  tail call void @_ZTH1x()
  br label %done

done:
  %o = load i32, ptr getelementptr inbounds nuw (i8, ptr @"x$tls", i64 8), align 8, !invariant.load !0
  %ox = zext i32 %o to i64
  %v = getelementptr inbounds i8, ptr %b, i64 %ox
  %r = load i32, ptr %v, align 4
  ret i32 %r
}

declare ptr @llvm.thread.pointer.p0()
declare dllimport void @_ZTH1x()

!0 = !{}
!1 = !{i64 4}
!2 = !{!"branch_weights", i32 1, i32 1048575}

;; Seven instructions and no call: the record's address, the index through
;; its first word, the block from the thread's array, the offset, the access.
; CHECK-LABEL: constant:
; CHECK:      movq __imp_x$tls@FUSED(%rip), %rax
; CHECK-NEXT: .Ltmp
; CHECK-NEXT: .reloc .Ltmp{{.*}}, IMAGE_REL_LLVM_FUSED_ACCESS, __imp_x$tls
; CHECK-NEXT: .site_fact 9, 0
; CHECK-NEXT: movq (%rax), %rcx
; CHECK-NEXT: .Ltmp
; CHECK-NEXT: .reloc .Ltmp{{.*}}, IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
; CHECK-NEXT: .site_fact 8, 0
; CHECK-NEXT: movl (%rcx), %ecx
; CHECK-NEXT: movq %gs:88, %rdx
; CHECK-NEXT: movq (%rdx,%rcx,8), %rcx
; CHECK-NEXT: .Ltmp
; CHECK-NEXT: .reloc .Ltmp{{.*}}, IMAGE_REL_LLVM_FUSED_ACCESS, __imp_x$tls
; CHECK-NEXT: .site_fact 9, 0
; CHECK-NEXT: movl 8(%rax), %eax
; CHECK-NEXT: .Ltmp
; CHECK-NEXT: .reloc .Ltmp{{.*}}, IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
; CHECK-NEXT: .site_fact 8, 8
; CHECK-NEXT: movl (%rcx,%rax), %eax
; CHECK-NEXT: retq

;; A variable that a thread initializes adds the test of its guard byte and a
;; branch to the call of the initialization function, out of line. That block
;; loads the offset again, which is no other value, so the access after it is
;; an index still, and the first load one that indexes alone read.
; CHECK-LABEL: dynamic:
; CHECK-NOT:  {{pushq|subq}}
; CHECK:      movq (%rcx,%rax,8), %rax
; CHECK:      .site_fact 9, 0
; CHECK-NEXT: movl 8(%rdx), %ecx
; CHECK:      .site_fact 9, 0
; CHECK-NEXT: movl 12(%rdx), %edx
; CHECK-NEXT: .Ltmp
; CHECK-NEXT: .reloc .Ltmp{{.*}}, IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
; CHECK-NEXT: .site_fact 8, 12
; CHECK-NEXT: cmpb $0, (%rax,%rdx)
; CHECK-NEXT: je dynamic.cold
; CHECK:      .reloc .Ltmp{{.*}}, IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
; CHECK-NEXT: .site_fact 8, 8
; CHECK-NEXT: movl (%rax,%rcx), %eax
; CHECK-LABEL: dynamic.cold:
; CHECK:      movq __imp__ZTH1x@LOAD_CALL(%rip), %rax
; CHECK-NEXT: callq *%rax
; CHECK-NEXT: movq __imp_x$tls@FUSED(%rip), %rax
; CHECK-NEXT: .Ltmp
; CHECK-NEXT: .reloc .Ltmp{{.*}}, IMAGE_REL_LLVM_FUSED_ACCESS, __imp_x$tls
; CHECK-NEXT: .site_fact 9, 0
; CHECK-NEXT: movl 8(%rax), %ecx

;; An add that takes the thread's block from memory is no index: the load of
;; the offset is an access that states nothing more.
; CHECK-LABEL: address:
; CHECK:      movq %gs:88, %rdx
; CHECK-NEXT: .Ltmp
; CHECK-NEXT: .reloc .Ltmp{{.*}}, IMAGE_REL_LLVM_FUSED_ACCESS, __imp_x$tls
; CHECK-NEXT: movl 8(%rax), %eax
; CHECK-NEXT: addq (%rdx,%rcx,8), %rax

;; The add of an offset to a block that a register holds is an lea, whose
;; flags are no matter, and so an index: of its first register here.
; CHECK-LABEL: held:
; CHECK:      movl 8(%rax), %eax
; CHECK-NEXT: .Ltmp
; CHECK-NEXT: .reloc .Ltmp{{.*}}, IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
; CHECK-NEXT: .site_fact 8, 24
; CHECK-NEXT: leaq (%rax,%rdx), %rax

;; Where the access after the test loads the offset, the block that calls
;; loads the record's address again for it. That load may reach an access
;; that is the load of a word, wherever it is, and the first load of the
;; address is still a fused read.
; CHECK-LABEL: sunk:
; CHECK:      movq __imp_x$tls@FUSED(%rip), %rax
; CHECK:      je sunk.cold
; CHECK-NEXT: .LBB
; CHECK-NEXT: .Ltmp
; CHECK-NEXT: .reloc .Ltmp{{.*}}, IMAGE_REL_LLVM_FUSED_ACCESS, __imp_x$tls
; CHECK-NEXT: .site_fact 9, 0
; CHECK-NEXT: movl 8(%rax), %eax
; CHECK-NEXT: .Ltmp
; CHECK-NEXT: .reloc .Ltmp{{.*}}, IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
; CHECK-NEXT: .site_fact 8, 8
; CHECK-NEXT: movl (%rcx,%rax), %eax
; CHECK-LABEL: sunk.cold:
; CHECK:      movq __imp_x$tls(%rip), %rax
; CHECK-NEXT: addq $40, %rsp
; CHECK-NEXT: jmp

;; The record's address, the index through its first word, the block from the
;; thread's array, the offset, the access, and no call.
; A64-LABEL: constant:
; A64:      adrp x8, __imp_x$tls
; A64-NEXT: ldr x8, [x8, :fused_lo12:__imp_x$tls]
; A64-NEXT: ldr x10, [x18, #88]
; A64-NEXT: .Ltmp
; A64-NEXT: .reloc .Ltmp{{.*}}, IMAGE_REL_LLVM_PAGEOFFSET_12L_IF_LOCAL, __imp_x$tls
; A64-NEXT: .site_fact 9, 0
; A64-NEXT: ldr x9, [x8]
; A64-NEXT: .Ltmp
; A64-NEXT: .reloc .Ltmp{{.*}}, IMAGE_REL_LLVM_PAGEOFFSET_12L_IF_LOCAL, __imp_x$tls
; A64-NEXT: .site_fact 9, 0
; A64-NEXT: ldr w8, [x8, #8]
; A64-NEXT: .Ltmp
; A64-NEXT: .reloc .Ltmp{{.*}}, IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
; A64-NEXT: .site_fact 8, 0
; A64-NEXT: ldr w9, [x9]
; A64-NEXT: ldr x9, [x10, x9, lsl #3]
; A64-NEXT: .Ltmp
; A64-NEXT: .reloc .Ltmp{{.*}}, IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
; A64-NEXT: .site_fact 8, 8
; A64-NEXT: ldr w0, [x9, x8]
; A64-NEXT: ret

;; A variable that a thread initializes adds the test of its guard byte and a
;; branch to the call of the initialization function, out of line. The two
;; offsets are one load of a pair, which is an access and no load of a word,
;; and the block that calls keeps them on the stack.
; A64-LABEL: dynamic:
; A64:      .site_fact 8, 0
; A64-NEXT: ldr w8, [x8]
; A64-NEXT: ldr x8, [x10, x8, lsl #3]
; A64-NEXT: .Ltmp
; A64-NEXT: .reloc .Ltmp{{.*}}, IMAGE_REL_LLVM_PAGEOFFSET_12L_IF_LOCAL, __imp_x$tls
; A64-NEXT: ldp w9, w10, [x9, #8]
; A64-NEXT: ldrb w10, [x8, x10]
; A64-NEXT: cbz w10,
; A64:      ldr w0, [x8, x9]
; A64-LABEL: dynamic.cold:
; A64:      ldr x10, [x10, :fused_lo12:__imp__ZTH1x]
; A64:      blr x10

;; The add of the offset to the block is an index.
; A64-LABEL: address:
; A64:      ldr x9, [x10, x9, lsl #3]
; A64-NEXT: .Ltmp
; A64-NEXT: .reloc .Ltmp{{.*}}, IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
; A64-NEXT: .site_fact 8, 8
; A64-NEXT: add x0, x9, x8

;; Where the access after the test loads the offset, the block that calls
;; loads the record's address again for it. That load may reach an access
;; that is the load of a word, wherever it is, and the first load of the
;; address is still a fused read.
; A64-LABEL: sunk:
; A64:      ldr x8, [x8, :fused_lo12:__imp_x$tls]
; A64:      cbz w10, {{.*}}
; A64-NEXT: .LBB
; A64-NEXT: .Ltmp
; A64-NEXT: .reloc .Ltmp{{.*}}, IMAGE_REL_LLVM_PAGEOFFSET_12L_IF_LOCAL, __imp_x$tls
; A64-NEXT: .site_fact 9, 0
; A64-NEXT: ldr w8, [x8, #8]
; A64-NEXT: .Ltmp
; A64-NEXT: .reloc .Ltmp{{.*}}, IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
; A64-NEXT: .site_fact 8, 8
; A64-NEXT: ldr w0, [x9, x8]
; A64-LABEL: sunk.cold:
; A64:      ldr x8, [x8, :lo12:__imp_x$tls]
; A64-NEXT: b

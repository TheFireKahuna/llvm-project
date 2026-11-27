; RUN: llc -mtriple=x86_64-windows-itanium < %s | FileCheck %s
; RUN: %if aarch64-registered-target %{ llc -mtriple=aarch64-windows-itanium < %s | FileCheck %s --check-prefix=A64 %}

;; A thread-local variable that may be another image's is reached through its
;; record x$tls: the loads of its words are accesses through its pointer, each
;; instruction that reads a word a fused index, with a fact naming the word and
;; whether its register is the first addend.

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

;; The back end lowers a thread-local variable that is not DSO local so: one
;; that the module only declares, or defines with a linkage under which
;; another image's definition may stand for it. A variable of the image is
;; its TLS index and its offset.
@v = external thread_local global i32
@l = external dso_local thread_local global i32

define i32 @symbolic() nounwind {
  %a = call ptr @llvm.threadlocal.address.p0(ptr @v)
  %r = load i32, ptr %a, align 4
  ret i32 %r
}

define i32 @local() nounwind {
  %a = call ptr @llvm.threadlocal.address.p0(ptr @l)
  %r = load i32, ptr %a, align 4
  ret i32 %r
}

;; A function loads the TLS index, the thread's array and the image's block
;; in it once, for all of its blocks.
@l2 = external dso_local thread_local global i32

define void @blocks(i1 %c, ptr %p) nounwind {
  %a = call ptr @llvm.threadlocal.address.p0(ptr @l)
  %x = load i32, ptr %a, align 4
  store i32 %x, ptr %p, align 4
  br i1 %c, label %t, label %e
t:
  %b = call ptr @llvm.threadlocal.address.p0(ptr @l2)
  store i32 %x, ptr %b, align 4
  ret void
e:
  ret void
}

;; The linker binds a function's sites as one, so code split out of line reads
;; the word that the hot part loaded as an index, and the load is one that only
;; indexes read.
define i32 @crossed() nounwind {
  %p = load ptr, ptr @"x$tls", align 8, !invariant.load !0, !dereferenceable !1, !align !1, !noundef !0
  %i = load i32, ptr %p, align 4, !invariant.load !0
  %a = tail call ptr @llvm.thread.pointer.p0()
  %ix = zext i32 %i to i64
  %s = getelementptr inbounds ptr, ptr %a, i64 %ix
  %b = load ptr, ptr %s, align 8, !invariant.load !0
  %g = load i32, ptr getelementptr inbounds nuw (i8, ptr @"x$tls", i64 12), align 4, !invariant.load !0
  %gx = zext i32 %g to i64
  %gp = getelementptr inbounds i8, ptr %b, i64 %gx
  %v = load i8, ptr %gp, align 1
  %c = icmp eq i8 %v, 0
  br i1 %c, label %cold, label %hot, !prof !2
cold:
  store i8 1, ptr %gp, align 1
  br label %hot
hot:
  %o = load i32, ptr getelementptr inbounds nuw (i8, ptr @"x$tls", i64 8), align 8, !invariant.load !0
  %ox = zext i32 %o to i64
  %vp = getelementptr inbounds i8, ptr %b, i64 %ox
  %r = load i32, ptr %vp, align 4
  ret i32 %r
}

;; Two chains of records give one block where both records are the image's.
;; The second's loads of the index and of the block, the block's moved up to
;; the index's, are marked with the first's pointer, and the index's facts
;; state the register that holds the first's block at the second's indexes.
@v2 = external thread_local global i32

;; A guard that the variable of its comdat states is reached through that
;; variable's record, whose last word is the guard's offset.
$t = comdat any
@t = linkonce_odr thread_local global i32 0, comdat, !tls_record !3
@g = linkonce_odr thread_local global i64 0, comdat($t), !tls_record !3

define i32 @guarded() nounwind {
  %gp = call ptr @llvm.threadlocal.address.p0(ptr @g)
  %byte = load i8, ptr %gp, align 8
  %unset = icmp eq i8 %byte, 0
  br i1 %unset, label %init, label %done, !prof !2
init:
  store i8 1, ptr %gp, align 8
  br label %done
done:
  %tp = call ptr @llvm.threadlocal.address.p0(ptr @t)
  %v = load i32, ptr %tp, align 4
  ret i32 %v
}

declare ptr @llvm.threadlocal.address.p0(ptr)
declare ptr @llvm.thread.pointer.p0()
declare dllimport void @_ZTH1x()

!0 = !{}
!1 = !{i64 4}
!2 = !{!"branch_weights", i32 1, i32 1048575}
!3 = !{ptr @g}

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
; CHECK:      movq (%rdx,%rax,8), %rax
; CHECK:      .site_fact 9, 0
; CHECK-NEXT: movl 8(%rcx), %edx
; CHECK:      .site_fact 9, 0
; CHECK-NEXT: movl 12(%rcx), %ecx
; CHECK-NEXT: .Ltmp
; CHECK-NEXT: .reloc .Ltmp{{.*}}, IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
; CHECK-NEXT: .site_fact 8, 12
; CHECK-NEXT: cmpb $0, (%rax,%rcx)
; CHECK-NEXT: je dynamic.cold
; CHECK:      .reloc .Ltmp{{.*}}, IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
; CHECK-NEXT: .site_fact 8, 8
; CHECK-NEXT: movl (%rax,%rdx), %eax
; CHECK-LABEL: dynamic.cold:
; CHECK:      movq __imp__ZTH1x@LOAD_CALL(%rip), %rax
; CHECK-NEXT: callq *%rax
; CHECK-NEXT: movq __imp_x$tls@FUSED(%rip), %rax
; CHECK-NEXT: .Ltmp
; CHECK-NEXT: .reloc .Ltmp{{.*}}, IMAGE_REL_LLVM_FUSED_ACCESS, __imp_x$tls
; CHECK-NEXT: .site_fact 9, 0
; CHECK-NEXT: movl 8(%rax), %edx

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
;; that is the load of a word, and both loads of the address are fused reads
;; of the function's code.
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
; CHECK:      movq __imp_x$tls@FUSED(%rip), %rax
; CHECK-NEXT: addq $40, %rsp
; CHECK-NEXT: jmp

; CHECK-LABEL: symbolic:
; CHECK:      movq __imp_v$tls@FUSED(%rip), %rax
; CHECK:      .site_fact 9, 0
; CHECK-NEXT: movq (%rax), %rcx
; CHECK:      .site_fact 8, 0
; CHECK-NEXT: movslq (%rcx), %rcx
; CHECK-NEXT: movq %gs:88, %rdx
; CHECK-NEXT: movq (%rdx,%rcx,8), %rcx
; CHECK:      .site_fact 9, 0
; CHECK-NEXT: movl 8(%rax), %eax
; CHECK-NEXT: .Ltmp
; CHECK-NEXT: .reloc .Ltmp{{.*}}, IMAGE_REL_LLVM_FUSED_INDEX, __imp_v$tls
; CHECK-NEXT: .site_fact 8, 8
; CHECK-NEXT: movl (%rcx,%rax), %eax
; CHECK-NEXT: retq
; CHECK-LABEL: local:
; CHECK:      movslq _tls_index(%rip), %rax
; CHECK:      movl l@SECREL32(%rax), %eax
; CHECK-LABEL: blocks:
; CHECK:      movslq _tls_index(%rip), %rax
; CHECK-NEXT: movq %gs:88, %r8
; CHECK-NEXT: movq (%r8,%rax,8), %rax
; CHECK:      je
; CHECK-NEXT: # %bb.1:
; CHECK-NEXT: movl %r8d, l2@SECREL32(%rax)
; CHECK-LABEL: crossed:
; CHECK:      .site_fact 8, 0
; CHECK:      .site_fact 9, 0
; CHECK-NEXT: movl 12(%rax), %edx
; CHECK:      je crossed.cold
; CHECK-LABEL: crossed.cold:
; CHECK-NEXT: .Ltmp
; CHECK-NEXT: .reloc .Ltmp{{.*}}, IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
; CHECK-NEXT: .site_fact 8, 12
; CHECK-NEXT: movb $1, (%rcx,%rdx)

;; The record's address, the index through its first word, the block from the
;; thread's array, the offset, the access, and no call.
; A64-LABEL: constant:
; A64:      ldr x10, [x18, #88]
; A64-NEXT: adrp x8, __imp_x$tls
; A64-NEXT: ldr x8, [x8, :fused_lo12:__imp_x$tls]
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
;; that is the load of a word, and both loads of the address are fused reads
;; of the function's code.
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
; A64:      ldr x8, [x8, :fused_lo12:__imp_x$tls]
; A64-NEXT: b

; A64-LABEL: symbolic:
; A64:      ldr x10, [x18, #88]
; A64-NEXT: adrp x8, __imp_v$tls
; A64-NEXT: ldr x8, [x8, :fused_lo12:__imp_v$tls]
; A64:      .site_fact 9, 0
; A64-NEXT: ldr x9, [x8]
; A64:      .site_fact 9, 0
; A64-NEXT: ldr w8, [x8, #8]
; A64:      .site_fact 8, 0
; A64-NEXT: ldrsw x9, [x9]
; A64-NEXT: ldr x9, [x10, x9, lsl #3]
; A64-NEXT: .Ltmp
; A64-NEXT: .reloc .Ltmp{{.*}}, IMAGE_REL_LLVM_FUSED_INDEX, __imp_v$tls
; A64-NEXT: .site_fact 8, 8
; A64-NEXT: ldr w0, [x9, x8]
; A64-NEXT: ret
; A64-LABEL: local:
; A64:      adrp x8, _tls_index
; A64:      ldr w0, [x8, :secrel_lo12:l]
; A64-LABEL: blocks:
; A64:      ldrsw x8, [x8, :lo12:_tls_index]
; A64-NEXT: ldr x8, [x9, x8, lsl #3]
; A64:      tbz
; A64-NEXT: // %bb.1:
; A64-NEXT: add x8, x8, :secrel_hi12:l2
; A64-NEXT: str w9, [x8, :secrel_lo12:l2]
; A64-LABEL: crossed:
; A64:      .site_fact 8, 0
; A64:      .site_fact 9, 0
; A64-NEXT: ldr w10, [x8, #12]
; A64-LABEL: crossed.cold:
; A64-NEXT: mov w11, #1
; A64-NEXT: .Ltmp
; A64-NEXT: .reloc .Ltmp{{.*}}, IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
; A64-NEXT: .site_fact 8, 12
; A64-NEXT: strb w11, [x9, x10]
;; The second chain's block load writes the register that holds the first's,
;; which the image's dropping it leaves holding that block; its index moves
;; down to it.
; CHECK-LABEL: guarded:
; CHECK:      movq __imp_t$tls@FUSED(%rip), %rax
; CHECK:      .reloc .Ltmp{{.*}}, IMAGE_REL_LLVM_FUSED_INDEX, __imp_t$tls
; CHECK-NEXT: .site_fact 8, 0
; CHECK-NEXT: movslq (%rcx), %rcx
; CHECK:      movl 12(%rax), %edx
; CHECK-NEXT: .Ltmp
; CHECK-NEXT: .reloc .Ltmp{{.*}}, IMAGE_REL_LLVM_FUSED_INDEX, __imp_t$tls
; CHECK-NEXT: .site_fact 8, 12
; CHECK-NEXT: cmpb $0, (%rcx,%rdx)
; CHECK:      movl 8(%rax), %eax
; CHECK-NEXT: .Ltmp
; CHECK-NEXT: .reloc .Ltmp{{.*}}, IMAGE_REL_LLVM_FUSED_INDEX, __imp_t$tls
; CHECK-NEXT: .site_fact 8, 8
; CHECK-NEXT: movl (%rcx,%rax), %eax
; CHECK-NOT:  __imp_g$tls
; A64-LABEL: guarded:
; A64:      ldr x8, [x8, :fused_lo12:__imp_t$tls]
; A64:      ldr w10, [x8, #12]
; A64-NEXT: .Ltmp
; A64-NEXT: .reloc .Ltmp{{.*}}, IMAGE_REL_LLVM_FUSED_INDEX, __imp_t$tls
; A64-NEXT: .site_fact 8, 12
; A64-NEXT: ldrb w11, [x9, x10]
; A64-NOT:  __imp_g$tls

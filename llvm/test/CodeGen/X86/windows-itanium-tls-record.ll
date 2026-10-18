; RUN: llc < %s -mtriple=x86_64-unknown-windows-itanium | FileCheck %s
; RUN: llc < %s -mtriple=x86_64-pc-windows-ntposix | FileCheck %s
; RUN: llc < %s -mtriple=x86_64-unknown-windows-itanium -O0 | \
; RUN:   FileCheck --check-prefix=O0 %s
; RUN: llc < %s -mtriple=x86_64-pc-windows-msvc | FileCheck --check-prefix=MSVC %s

; On Windows Itanium and NT-POSIX a thread-local variable that is not dso_local
; may be another image's, whether it is declared or defined weakly, and is
; reached through the record whose address the import pointer of <name>$tls
; holds: the address of the defining image's TLS index, then the variable's
; offset in that image's template. The record's loads are invariant, so they
; leave a loop, while the thread's TLS array is reloaded at each access. Each
; load from the record is described to the linker as an access of the record's
; load and as a load itself, with the instructions that read its register:
; the load of the TLS index through the address, and the memory operands that
; add the offset, so that where the record is in the image the linker can make
; the code a variable of the image gets. A dso_local variable is reached
; directly, and the MSVC target is unchanged.

$comdatvar = comdat any

@var = external thread_local global i32
@comdatvar = linkonce_odr thread_local global i32 1, comdat
@localvar = dso_local thread_local global i32 2
@array = external thread_local global [4 x i32]

define i32 @getVar() {
; CHECK-LABEL: getVar:
; CHECK:       [[REC:\.Lfused[0-9]+]]:
; CHECK-NEXT:    .linkfusedread
; CHECK-NEXT:    movq __imp_var$tls(%rip), %rax
; CHECK-NEXT:    .linkfusedaccess [[REC]], %rax
; CHECK-NEXT:  [[ADDR:\.Lfused[0-9]+]]:
; CHECK-NEXT:    .linkfusedread
; CHECK-NEXT:    movq (%rax), %rcx
; CHECK-NEXT:    .linkfusedaccess [[ADDR]], %rcx
; CHECK-NEXT:    movl (%rcx), %ecx
; CHECK-NEXT:    movq %gs:88, %rdx
; CHECK-NEXT:    movq (%rdx,%rcx,8), %rcx
; CHECK-NEXT:    .linkfusedaccess [[REC]], %rax
; CHECK-NEXT:  [[OFF:\.Lfused[0-9]+]]:
; CHECK-NEXT:    .linkfusedread
; CHECK-NEXT:    movl 8(%rax), %eax
; CHECK-NEXT:    .linkfusedindex [[OFF]], %rax
; CHECK-NEXT:    movl (%rcx,%rax), %eax
; O0-LABEL:    getVar:
; O0:            movq __imp_var$tls(%rip), %rcx
; O0-NEXT:       movq (%rcx), %rax
; O0-NEXT:       movl (%rax), %eax
; O0:            movq %gs:88, %rax
; O0-NEXT:       movq (%rax,%rdx,8), %rax
; O0-NEXT:       movl 8(%rcx), %ecx
; MSVC-LABEL:  getVar:
; MSVC:          movl _tls_index(%rip), %eax
; MSVC:          movl var@SECREL32(%rax), %eax
  %p = call ptr @llvm.threadlocal.address.p0(ptr @var)
  %v = load i32, ptr %p
  ret i32 %v
}

define i32 @getComdatVar() {
; CHECK-LABEL: getComdatVar:
; CHECK:         movq __imp_comdatvar$tls(%rip), %rax
; CHECK:         movq (%rax), %rcx
; CHECK:         movl 8(%rax), %eax
; CHECK-NEXT:    .linkfusedindex {{.*}}, %rax
; CHECK-NEXT:    movl (%rcx,%rax), %eax
  %p = call ptr @llvm.threadlocal.address.p0(ptr @comdatvar)
  %v = load i32, ptr %p
  ret i32 %v
}

define i32 @getLocalVar() {
; CHECK-LABEL: getLocalVar:
; CHECK:         movl _tls_index(%rip), %eax
; CHECK-NEXT:    movq %gs:88, %rcx
; CHECK-NEXT:    movq (%rcx,%rax,8), %rax
; CHECK-NEXT:    movl localvar@SECREL32(%rax), %eax
  %p = call ptr @llvm.threadlocal.address.p0(ptr @localvar)
  %v = load i32, ptr %p
  ret i32 %v
}

define i32 @getElement() {
; CHECK-LABEL: getElement:
; CHECK:         movq __imp_array$tls(%rip), %rax
; CHECK:         movl 8(%rax), %eax
; CHECK-NEXT:    .linkfusedindex {{.*}}, %rax
; CHECK-NEXT:    movl 8(%rcx,%rax), %eax
  %p = call ptr @llvm.threadlocal.address.p0(ptr @array)
  %e = getelementptr inbounds [4 x i32], ptr %p, i64 0, i64 2
  %v = load i32, ptr %e
  ret i32 %v
}

define i32 @sum(i32 %n) {
; The offset, loaded once, is described with the access in the loop that adds
; it.
; CHECK-LABEL: sum:
; CHECK:         movq __imp_var$tls(%rip), %[[REC:r[a-z0-9]+]]
; CHECK:         movq (%[[REC]]), %[[IDX:r[a-z0-9]+]]
; CHECK:         movl (%[[IDX]]), %[[IDX32:e[a-z0-9]+|r[0-9]+d]]
; CHECK:       [[OFF:\.Lfused[0-9]+]]:
; CHECK-NEXT:    .linkfusedread
; CHECK-NEXT:    movl 8(%[[REC]]), %[[OFF32:e[a-z0-9]+|r[0-9]+d]]
; CHECK:       .LBB4_1:
; CHECK:         movq %gs:88, %[[BLK:r[a-z0-9]+]]
; CHECK-NEXT:    movq (%[[BLK]],%[[IDX]],8), %[[BLK]]
; CHECK-NEXT:    .linkfusedindex [[OFF]], %[[REC]]
; CHECK-NEXT:    addl (%[[BLK]],%[[REC]]), %eax
; CHECK:         jb .LBB4_1
entry:
  br label %loop

loop:
  %i = phi i32 [ 0, %entry ], [ %i.next, %loop ]
  %acc = phi i32 [ 0, %entry ], [ %acc.next, %loop ]
  %p = call ptr @llvm.threadlocal.address.p0(ptr @var)
  %v = load volatile i32, ptr %p
  %acc.next = add i32 %acc, %v
  %i.next = add i32 %i, 1
  %more = icmp ult i32 %i.next, %n
  br i1 %more, label %loop, label %exit

exit:
  ret i32 %acc.next
}

declare ptr @llvm.threadlocal.address.p0(ptr)

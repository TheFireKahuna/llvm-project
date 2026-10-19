; RUN: llc < %s -mtriple=x86_64-unknown-windows-itanium | FileCheck %s
; RUN: llc < %s -mtriple=x86_64-pc-windows-ntposix | FileCheck %s
; RUN: llc < %s -mtriple=x86_64-unknown-windows-itanium -enable-new-pm | \
; RUN:   FileCheck %s
; RUN: llc < %s -mtriple=x86_64-unknown-windows-itanium -verify-machineinstrs \
; RUN:   -o /dev/null
; RUN: llc < %s -mtriple=x86_64-unknown-windows-itanium -O0 | \
; RUN:   FileCheck %s --check-prefix=NONE
; RUN: llc < %s -mtriple=x86_64-pc-windows-msvc | \
; RUN:   FileCheck %s --check-prefix=NONE

; A load of the import pointer of data that never changes, whose register only
; reads of memory read, as their base with any index, is described as such, so
; that the linker can make it take the address of the image's own copy where
; another image provides the data. Its address must not escape: a store or a
; return of it keeps the load as it is, as does a volatile read, and data that
; may change is never described. An atomic read of data that nothing writes
; reads the copy as well, and so does a read through an alias of the data.

; NONE-NOT: .linkvalueonly

$tbl = comdat any
$mut = comdat any
@tbl = linkonce_odr constant [4 x i32] [i32 1, i32 2, i32 3, i32 4], comdat
@mut = linkonce_odr global [4 x i32] [i32 1, i32 2, i32 3, i32 4], comdat
@ext = external constant [4 x i32]
@tblalias = alias [4 x i32], ptr @tbl
@sink = external global ptr

; CHECK-LABEL: indexed:
; CHECK:       .linkvalueonly
; CHECK-NEXT:  movq __imp_tbl(%rip), %[[R:[a-z0-9]+]]
; CHECK-NEXT:  movl (%[[R]],%{{[a-z0-9]+}},4), %eax
define i32 @indexed(i64 %i) {
  %p = getelementptr inbounds [4 x i32], ptr @tbl, i64 0, i64 %i
  %v = load i32, ptr %p
  ret i32 %v
}

; CHECK-LABEL: aliased:
; CHECK:       .linkvalueonly
; CHECK-NEXT:  movq __imp_tblalias(%rip), %[[R:[a-z0-9]+]]
; CHECK-NEXT:  movl (%[[R]],%{{[a-z0-9]+}},4), %eax
define i32 @aliased(i64 %i) {
  %p = getelementptr inbounds [4 x i32], ptr @tblalias, i64 0, i64 %i
  %v = load i32, ptr %p
  ret i32 %v
}

; CHECK-LABEL: two:
; CHECK:       .linkvalueonly
; CHECK-NEXT:  movq __imp_tbl(%rip), %[[R:[a-z0-9]+]]
; CHECK-NEXT:  movl (%[[R]],%{{[a-z0-9]+}},4), %eax
; CHECK-NEXT:  addl (%[[R]],%{{[a-z0-9]+}},4), %eax
define i32 @two(i64 %i, i64 %j) {
  %p = getelementptr inbounds [4 x i32], ptr @tbl, i64 0, i64 %i
  %v = load i32, ptr %p
  %q = getelementptr inbounds [4 x i32], ptr @tbl, i64 0, i64 %j
  %w = load i32, ptr %q
  %s = add i32 %v, %w
  ret i32 %s
}

; CHECK-LABEL: stored:
; CHECK-NOT:   .linkvalueonly
; CHECK:       retq
define void @stored() {
  store ptr @tbl, ptr @sink
  ret void
}

; CHECK-LABEL: returned:
; CHECK-NOT:   .linkvalueonly
; CHECK:       retq
define ptr @returned(i64 %i) {
  %p = getelementptr inbounds [4 x i32], ptr @tbl, i64 0, i64 %i
  ret ptr %p
}

; CHECK-LABEL: volatile:
; CHECK-NOT:   .linkvalueonly
; CHECK:       retq
define i32 @volatile(i64 %i) {
  %p = getelementptr inbounds [4 x i32], ptr @tbl, i64 0, i64 %i
  %v = load volatile i32, ptr %p
  ret i32 %v
}

; CHECK-LABEL: atomic:
; CHECK:       .linkvalueonly
; CHECK-NEXT:  movq __imp_tbl(%rip),
define i32 @atomic(i64 %i) {
  %p = getelementptr inbounds [4 x i32], ptr @tbl, i64 0, i64 %i
  %v = load atomic i32, ptr %p acquire, align 4
  ret i32 %v
}

; Data that may change, or with no definition here, which no copy can serve.
; CHECK-LABEL: mutable:
; CHECK-NOT:   .linkvalueonly
; CHECK:       retq
define i32 @mutable(i64 %i) {
  %p = getelementptr inbounds [4 x i32], ptr @mut, i64 0, i64 %i
  %v = load i32, ptr %p
  ret i32 %v
}

; CHECK-LABEL: declared:
; CHECK-NOT:   .linkvalueonly
; CHECK:       retq
define i32 @declared(i64 %i) {
  %p = getelementptr inbounds [4 x i32], ptr @ext, i64 0, i64 %i
  %v = load i32, ptr %p
  ret i32 %v
}

; The address may also flow through the function, so long as nothing but
; reads of memory and comparisons with addresses within the data use it, or
; callees that keep nothing of it; every load of the import pointer is then
; described, together, so that addresses that different loads lead to still
; compare as before.
@other = external global i32
declare void @reads(ptr captures(none) readonly)
declare void @keeps(ptr)

; CHECK-LABEL: loop:
; CHECK:       .linkvalueonly
; CHECK-NEXT:  movq __imp_tbl(%rip),
define i32 @loop() {
entry:
  br label %loop
loop:
  %p = phi ptr [ @tbl, %entry ], [ %n, %loop ]
  %s = phi i32 [ 0, %entry ], [ %t, %loop ]
  %v = load i32, ptr %p
  %t = add i32 %s, %v
  %n = getelementptr inbounds i32, ptr %p, i64 1
  %d = icmp eq ptr %n, getelementptr inbounds (i8, ptr @tbl, i64 16)
  br i1 %d, label %exit, label %loop
exit:
  ret i32 %t
}

; CHECK-LABEL: passed:
; CHECK:       .linkvalueonly
; CHECK-NEXT:  movq __imp_tbl(%rip), %{{[a-z]+}}
; CHECK-NEXT:  callq reads
define void @passed() {
  call void @reads(ptr @tbl)
  ret void
}

; CHECK-LABEL: offset:
; CHECK:       .linkvalueonly
; CHECK-NEXT:  movq __imp_tbl(%rip), %rax
; CHECK:       callq reads
; CHECK:       .linkvalueonly
; CHECK-NEXT:  movq __imp_tbl(%rip), %rax
define i32 @offset(i64 %i) {
  %p = getelementptr inbounds [4 x i32], ptr @tbl, i64 0, i64 %i
  %q = getelementptr inbounds i8, ptr %p, i64 4
  call void @reads(ptr %q)
  %v = load i32, ptr %q
  ret i32 %v
}

; A comparison with another address, or a callee that may keep it, takes the
; address of the data, which every image must share.
; CHECK-LABEL: foreign:
; CHECK-NOT:   .linkvalueonly
; CHECK:       retq
define i1 @foreign(i64 %i) {
  %p = getelementptr inbounds [4 x i32], ptr @tbl, i64 0, i64 %i
  %c = icmp eq ptr %p, @other
  ret i1 %c
}

; CHECK-LABEL: kept:
; CHECK-NOT:   .linkvalueonly
; CHECK:       retq
define void @kept() {
  call void @keeps(ptr @tbl)
  ret void
}

; Where the load stays, since its address is stored, an access through its
; register that only reads is described as such, so that the linker may make
; it read the copy directly while the store keeps reaching the instance.
; CHECK-LABEL: escapes:
; CHECK:       .linkfusedaccess .Lfused{{[0-9]+}}, %rax
; CHECK-NEXT:  .linkvalueonly
; CHECK-NEXT:  movl 8(%rax), %eax
define i32 @escapes() {
  store ptr @tbl, ptr @sink
  %p = getelementptr inbounds i8, ptr @tbl, i64 8
  %v = load i32, ptr %p
  ret i32 %v
}

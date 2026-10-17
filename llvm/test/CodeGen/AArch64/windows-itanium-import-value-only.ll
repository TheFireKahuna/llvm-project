; RUN: llc < %s -mtriple=aarch64-unknown-windows-itanium | FileCheck %s
; RUN: llc < %s -mtriple=aarch64-pc-windows-ntposix | FileCheck %s
; RUN: llc < %s -mtriple=aarch64-unknown-windows-itanium -verify-machineinstrs \
; RUN:   -o /dev/null
; RUN: llc < %s -mtriple=aarch64-pc-windows-msvc | \
; RUN:   FileCheck %s --check-prefix=NONE

; An ldr of the import pointer of data that never changes, whose register only
; reads of memory read, as their base with any offset or index, is described
; as such with the adrp that gives it the pointer's page, so that the linker
; can make the pair take the address of the image's own copy where another
; image provides the data. Its address must not escape, nor be written back,
; and a volatile read keeps the load as it is, as does data that may change.

; NONE-NOT: .linkvalueonly

$tbl = comdat any
$mut = comdat any
@tbl = linkonce_odr constant [4 x i32] [i32 1, i32 2, i32 3, i32 4], comdat
@mut = linkonce_odr global [4 x i32] [i32 1, i32 2, i32 3, i32 4], comdat
@sink = external global ptr

; CHECK-LABEL: indexed:
; CHECK:       .Lpage[[#N:]]:
; CHECK-NEXT:  adrp x8, __imp_tbl
; CHECK-NEXT:  .linkvalueonly .Lpage[[#N]]
; CHECK-NEXT:  ldr x8, [x8, :lo12:__imp_tbl]
; CHECK-NEXT:  ldr w0, [x8, x0, lsl #2]
define i32 @indexed(i64 %i) {
  %p = getelementptr inbounds [4 x i32], ptr @tbl, i64 0, i64 %i
  %v = load i32, ptr %p
  ret i32 %v
}

; The second read overwrites the register, which holds no address after it.
; CHECK-LABEL: two:
; CHECK:       .linkvalueonly .Lpage
; CHECK-NEXT:  ldr x8, [x8, :lo12:__imp_tbl]
; CHECK-NEXT:  ldr w9, [x8, x0, lsl #2]
; CHECK-NEXT:  ldr w8, [x8, x1, lsl #2]
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
; CHECK:       ret
define void @stored() {
  store ptr @tbl, ptr @sink
  ret void
}

; CHECK-LABEL: returned:
; CHECK-NOT:   .linkvalueonly
; CHECK:       ret
define ptr @returned(i64 %i) {
  %p = getelementptr inbounds [4 x i32], ptr @tbl, i64 0, i64 %i
  ret ptr %p
}

; CHECK-LABEL: volatile:
; CHECK-NOT:   .linkvalueonly
; CHECK:       ret
define i32 @volatile(i64 %i) {
  %p = getelementptr inbounds [4 x i32], ptr @tbl, i64 0, i64 %i
  %v = load volatile i32, ptr %p
  ret i32 %v
}

; CHECK-LABEL: mutable:
; CHECK-NOT:   .linkvalueonly
; CHECK:       ret
define i32 @mutable(i64 %i) {
  %p = getelementptr inbounds [4 x i32], ptr @mut, i64 0, i64 %i
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
; CHECK:       .linkvalueonly .Lpage
; CHECK-NEXT:  ldr x9, [x9, :lo12:__imp_tbl]
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
; CHECK:       .linkvalueonly .Lpage
; CHECK-NEXT:  ldr x0, [x0, :lo12:__imp_tbl]
; CHECK-NEXT:  bl reads
define void @passed() {
  call void @reads(ptr @tbl)
  ret void
}

; CHECK-LABEL: offset:
; CHECK:       .linkvalueonly .Lpage
; CHECK-NEXT:  ldr x8, [x8, :lo12:__imp_tbl]
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
; CHECK:       ret
define i1 @foreign(i64 %i) {
  %p = getelementptr inbounds [4 x i32], ptr @tbl, i64 0, i64 %i
  %c = icmp eq ptr %p, @other
  ret i1 %c
}

; CHECK-LABEL: kept:
; CHECK-NOT:   .linkvalueonly
; CHECK:       bl keeps
define void @kept() {
  call void @keeps(ptr @tbl)
  ret void
}

; Where the load stays, since its address is stored, an access through its
; register that only reads is described as such, so that the linker may make
; it read the copy directly while the store keeps reaching the instance.
; CHECK-LABEL: escapes:
; CHECK:       .linkvalueonly{{$}}
; CHECK-NEXT:  .linkfusedaccess .Lfused{{[0-9]+}}, x8
; CHECK-NEXT:  ldr w0, [x8, #8]
define i32 @escapes() {
  store ptr @tbl, ptr @sink
  %p = getelementptr inbounds i8, ptr @tbl, i64 8
  %v = load i32, ptr %p
  ret i32 %v
}

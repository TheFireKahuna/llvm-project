; RUN: opt -S -passes=cfguard %s | FileCheck %s --check-prefixes=CHECK,ELIDE
; RUN: sed 's/x86_64-unknown-windows-itanium/x86_64-pc-windows-ntposix/' %s \
; RUN:   | opt -S -passes=cfguard | FileCheck %s --check-prefixes=CHECK,ELIDE
; RUN: sed 's/x86_64-unknown-windows-itanium/aarch64-unknown-windows-itanium/' %s \
; RUN:   | opt -S -passes=cfguard | FileCheck %s --check-prefixes=CHECK,ELIDE
; RUN: sed 's/x86_64-unknown-windows-itanium/x86_64-pc-windows-msvc/' %s \
; RUN:   | opt -S -passes=cfguard | FileCheck %s --check-prefixes=CHECK,KEEP

;; An indirect call whose target is loaded from a constant table of functions,
;; at an index proven in range without relying on inbounds or any other
;; undefined behaviour, can only reach one of the table's entries. On Windows
;; Itanium and NT-POSIX the load is marked, and the call keeps its checks,
;; which the backend drops if the target stays in its register until the call.
;; Other triples mark nothing.

target triple = "x86_64-unknown-windows-itanium"

declare !kcfi_type !3 void @f()
declare !kcfi_type !3 void @g()
declare !kcfi_type !4 void @h()
declare void @u()
declare !kcfi_type !3 dllimport void @imp()

@table = internal constant [256 x ptr] [
  ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g,
  ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g,
  ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g,
  ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g,
  ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g,
  ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g,
  ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g,
  ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g,
  ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g,
  ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g,
  ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g,
  ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g,
  ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g,
  ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g,
  ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g,
  ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g,
  ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g,
  ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g,
  ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g,
  ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g,
  ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g,
  ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g,
  ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g,
  ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g,
  ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g,
  ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g,
  ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g,
  ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g,
  ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g,
  ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g,
  ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g,
  ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g, ptr @f, ptr @g
]

@sparse = internal constant [4 x ptr] [ptr @f, ptr null, ptr @g, ptr null]
@writable = internal global [4 x ptr] [ptr @f, ptr @g, ptr @f, ptr @g]
@placed = internal constant [4 x ptr] [ptr @f, ptr @g, ptr @f, ptr @g], section ".data"
@imports = internal constant [4 x ptr] [ptr @f, ptr @imp, ptr @f, ptr @g]
@mixed = internal constant [4 x ptr] [ptr @f, ptr @h, ptr @f, ptr @g]
@untyped = internal constant [4 x ptr] [ptr @f, ptr @u, ptr @f, ptr @g]

;; Every value of a zero-extended i8 selects one of the 256 entries.
; CHECK-LABEL: define void @u8_index(
; ELIDE:         load ptr, ptr %p, align 8, !cfguard_proven
; KEEP-NOT:      !cfguard_proven
; CHECK:         call {{.*}}@__llvm_kcfi_{{dispatch|check}}_0000000c(
define void @u8_index(i8 %op) {
  %idx = zext i8 %op to i64
  %p = getelementptr inbounds [8 x i8], ptr @table, i64 %idx
  %f = load ptr, ptr %p
  call void %f() [ "kcfi"(i32 12) ]
  ret void
}

;; A zero-extended i16 can select a word past the end.
; CHECK-LABEL: define void @wide_index(
; CHECK-NOT:     !cfguard_proven
; CHECK:         call {{.*}}@__llvm_kcfi_{{dispatch|check}}_0000000c(
define void @wide_index(i16 %op) {
  %idx = zext i16 %op to i64
  %p = getelementptr inbounds [8 x i8], ptr @table, i64 %idx
  %f = load ptr, ptr %p
  call void %f() [ "kcfi"(i32 12) ]
  ret void
}

;; A bounds check on the edge into the block proves the index.
; CHECK-LABEL: define void @bounds_check(
; ELIDE:         load ptr, ptr %p, align 8, !cfguard_proven
; KEEP-NOT:      !cfguard_proven
; CHECK:         call {{.*}}@__llvm_kcfi_{{dispatch|check}}_0000000c(
define void @bounds_check(i32 %i) {
entry:
  %in = icmp ult i32 %i, 256
  br i1 %in, label %call, label %exit
call:
  %idx = zext i32 %i to i64
  %p = getelementptr inbounds [8 x i8], ptr @table, i64 %idx
  %f = load ptr, ptr %p
  call void %f() [ "kcfi"(i32 12) ]
  br label %exit
exit:
  ret void
}

;; The same on the false edge of the inverse compare.
; CHECK-LABEL: define void @bounds_check_false_edge(
; ELIDE:         load ptr, ptr %p, align 8, !cfguard_proven
; KEEP-NOT:      !cfguard_proven
; CHECK:         call {{.*}}@__llvm_kcfi_{{dispatch|check}}_0000000c(
define void @bounds_check_false_edge(i32 %i) {
entry:
  %out = icmp uge i32 %i, 256
  br i1 %out, label %exit, label %call
call:
  %idx = zext i32 %i to i64
  %p = getelementptr inbounds [8 x i8], ptr @table, i64 %idx
  %f = load ptr, ptr %p
  call void %f() [ "kcfi"(i32 12) ]
  br label %exit
exit:
  ret void
}

;; A same-sign compare may be lowered as a signed one, which a negative index
;; passes.
; CHECK-LABEL: define void @bounds_check_samesign(
; CHECK-NOT:     !cfguard_proven
; CHECK:         call {{.*}}@__llvm_kcfi_{{dispatch|check}}_0000000c(
define void @bounds_check_samesign(i32 %i) {
entry:
  %in = icmp samesign ult i32 %i, 256
  br i1 %in, label %call, label %exit
call:
  %idx = zext i32 %i to i64
  %p = getelementptr inbounds [8 x i8], ptr @table, i64 %idx
  %f = load ptr, ptr %p
  call void %f() [ "kcfi"(i32 12) ]
  br label %exit
exit:
  ret void
}

;; A callee could spill a bounds-checked index, or the loaded target, that is
;; live across a call.
; CHECK-LABEL: define void @call_after_check(
; CHECK-NOT:     !cfguard_proven
; CHECK:         call {{.*}}@__llvm_kcfi_{{dispatch|check}}_0000000c(
define void @call_after_check(i32 %i) {
entry:
  %in = icmp ult i32 %i, 256
  br i1 %in, label %call, label %exit
call:
  call void @g()
  %idx = zext i32 %i to i64
  %p = getelementptr inbounds [8 x i8], ptr @table, i64 %idx
  %f = load ptr, ptr %p
  call void %f() [ "kcfi"(i32 12) ]
  br label %exit
exit:
  ret void
}

; CHECK-LABEL: define void @call_after_load(
; CHECK-NOT:     !cfguard_proven
; CHECK:         call {{.*}}@__llvm_kcfi_{{dispatch|check}}_0000000c(
define void @call_after_load(i8 %op) {
  %idx = zext i8 %op to i64
  %p = getelementptr inbounds [8 x i8], ptr @table, i64 %idx
  %f = load ptr, ptr %p
  call void @g()
  call void %f() [ "kcfi"(i32 12) ]
  ret void
}

;; A table load hoisted out of a loop is live across the calls in its body.
; CHECK-LABEL: define void @hoisted_load(
; CHECK-NOT:     !cfguard_proven
; CHECK:         call {{.*}}@__llvm_kcfi_{{dispatch|check}}_0000000c(
define void @hoisted_load(i8 %op, i32 %n) {
entry:
  %idx = zext i8 %op to i64
  %p = getelementptr inbounds [8 x i8], ptr @table, i64 %idx
  %f = load ptr, ptr %p
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %i.next, %loop ]
  call void %f() [ "kcfi"(i32 12) ]
  %i.next = add i32 %i, 1
  %done = icmp eq i32 %i.next, %n
  br i1 %done, label %exit, label %loop
exit:
  ret void
}

;; inbounds only makes an out-of-range address poison; it proves nothing.
; CHECK-LABEL: define void @inbounds_only(
; CHECK-NOT:     !cfguard_proven
; CHECK:         call {{.*}}@__llvm_kcfi_{{dispatch|check}}_0000000c(
define void @inbounds_only(i64 %i) {
  %p = getelementptr inbounds [256 x ptr], ptr @table, i64 0, i64 %i
  %f = load ptr, ptr %p
  call void %f() [ "kcfi"(i32 12) ]
  ret void
}

;; The mark is on the load, so a second call of the loaded value, after a call
;; that may spill it, leaves the load unmarked.
; CHECK-LABEL: define void @second_call(
; CHECK-NOT:     !cfguard_proven
; CHECK:         call {{.*}}@__llvm_kcfi_{{dispatch|check}}_0000000c(
define void @second_call(i8 %op) {
  %idx = zext i8 %op to i64
  %p = getelementptr inbounds [8 x i8], ptr @table, i64 %idx
  %f = load ptr, ptr %p
  call void %f() [ "kcfi"(i32 12) ]
  call void %f() [ "kcfi"(i32 12) ]
  ret void
}

;; A null entry faults when called, so it needs no check either.
; CHECK-LABEL: define void @null_entry(
; ELIDE:         load ptr, ptr %p, align 8, !cfguard_proven
; KEEP-NOT:      !cfguard_proven
; CHECK:         call {{.*}}@__llvm_kcfi_{{dispatch|check}}_0000000c(
define void @null_entry(i64 %i) {
  %idx = and i64 %i, 3
  %p = getelementptr inbounds [8 x i8], ptr @sparse, i64 %idx
  %f = load ptr, ptr %p
  call void %f() [ "kcfi"(i32 12) ]
  ret void
}

;; Tables that may be written, or that name another image's function, are not
;; proof of the target.
; CHECK-LABEL: define void @writable_table(
; CHECK-NOT:     !cfguard_proven
; CHECK:         call {{.*}}@__llvm_kcfi_{{dispatch|check}}_0000000c(
define void @writable_table(i64 %i) {
  %idx = and i64 %i, 3
  %p = getelementptr inbounds [8 x i8], ptr @writable, i64 %idx
  %f = load ptr, ptr %p
  call void %f() [ "kcfi"(i32 12) ]
  ret void
}

; CHECK-LABEL: define void @placed_table(
; CHECK-NOT:     !cfguard_proven
; CHECK:         call {{.*}}@__llvm_kcfi_{{dispatch|check}}_0000000c(
define void @placed_table(i64 %i) {
  %idx = and i64 %i, 3
  %p = getelementptr inbounds [8 x i8], ptr @placed, i64 %idx
  %f = load ptr, ptr %p
  call void %f() [ "kcfi"(i32 12) ]
  ret void
}

; CHECK-LABEL: define void @dllimport_entry(
; CHECK-NOT:     !cfguard_proven
; CHECK:         call {{.*}}@__llvm_kcfi_{{dispatch|check}}_0000000c(
define void @dllimport_entry(i64 %i) {
  %idx = and i64 %i, 3
  %p = getelementptr inbounds [8 x i8], ptr @imports, i64 %idx
  %f = load ptr, ptr %p
  call void %f() [ "kcfi"(i32 12) ]
  ret void
}


;; A check of the call's type would reject an entry of another type, or one
;; with no type, so such a table proves nothing for a call with a kcfi bundle.
; CHECK-LABEL: define void @type_mismatch(
; CHECK-NOT:     !cfguard_proven
; CHECK:         call {{.*}}@__llvm_kcfi_{{dispatch|check}}_0000000c(
define void @type_mismatch(i64 %i) {
  %idx = and i64 %i, 3
  %p = getelementptr inbounds [8 x i8], ptr @mixed, i64 %idx
  %f = load ptr, ptr %p
  call void %f() [ "kcfi"(i32 12) ]
  ret void
}

; CHECK-LABEL: define void @untyped_entry(
; CHECK-NOT:     !cfguard_proven
; CHECK:         call {{.*}}@__llvm_kcfi_{{dispatch|check}}_0000000c(
define void @untyped_entry(i64 %i) {
  %idx = and i64 %i, 3
  %p = getelementptr inbounds [8 x i8], ptr @untyped, i64 %idx
  %f = load ptr, ptr %p
  call void %f() [ "kcfi"(i32 12) ]
  ret void
}

;; A call without a kcfi bundle checks no type, so any function will do.
; CHECK-LABEL: define void @untyped_call(
; ELIDE:         load ptr, ptr %p, align 8, !cfguard_proven
; KEEP-NOT:      !cfguard_proven
; CHECK:         load ptr, ptr @__guard_{{dispatch|check}}_icall_fptr
define void @untyped_call(i64 %i) {
  %idx = and i64 %i, 3
  %p = getelementptr inbounds [8 x i8], ptr @mixed, i64 %idx
  %f = load ptr, ptr %p
  call void %f()
  ret void
}

!llvm.module.flags = !{!0, !1, !2}
!0 = !{i32 4, !"kcfi", i32 1}
!1 = !{i32 4, !"function-type-prefix", i32 -559038737}
!2 = !{i32 2, !"cfguard", i32 2}
!3 = !{i32 12}
!4 = !{i32 13}

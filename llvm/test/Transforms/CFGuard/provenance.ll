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
;; Itanium and NT-POSIX it is left without a Control Flow Guard check and
;; loses its kcfi bundle. Other triples check it as before.

target triple = "x86_64-unknown-windows-itanium"

declare void @f()
declare void @g()
declare dllimport void @imp()

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

;; Every value of a zero-extended i8 selects one of the 256 entries.
; CHECK-LABEL: define void @u8_index(
; ELIDE-NOT:     {{__llvm_kcfi_|__guard_}}
; ELIDE:         call void %f(){{$}}
; KEEP:          call {{.*}}@__llvm_kcfi_{{dispatch|check}}_0000000c(
define void @u8_index(i8 %op) {
  %idx = zext i8 %op to i64
  %p = getelementptr inbounds [8 x i8], ptr @table, i64 %idx
  %f = load ptr, ptr %p
  call void %f() [ "kcfi"(i32 12) ]
  ret void
}

;; A zero-extended i16 can select a word past the end.
; CHECK-LABEL: define void @wide_index(
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
; ELIDE-NOT:     {{__llvm_kcfi_|__guard_}}
; ELIDE:         call void %f(){{$}}
; KEEP:          call {{.*}}@__llvm_kcfi_{{dispatch|check}}_0000000c(
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
; ELIDE-NOT:     {{__llvm_kcfi_|__guard_}}
; ELIDE:         call void %f(){{$}}
; KEEP:          call {{.*}}@__llvm_kcfi_{{dispatch|check}}_0000000c(
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
; CHECK:         call {{.*}}@__llvm_kcfi_{{dispatch|check}}_0000000c(
define void @inbounds_only(i64 %i) {
  %p = getelementptr inbounds [256 x ptr], ptr @table, i64 0, i64 %i
  %f = load ptr, ptr %p
  call void %f() [ "kcfi"(i32 12) ]
  ret void
}

;; A null entry faults when called, so it needs no check either.
; CHECK-LABEL: define void @null_entry(
; ELIDE-NOT:     {{__llvm_kcfi_|__guard_}}
; ELIDE:         call void %f(){{$}}
; KEEP:          call {{.*}}@__llvm_kcfi_{{dispatch|check}}_0000000c(
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
; CHECK:         call {{.*}}@__llvm_kcfi_{{dispatch|check}}_0000000c(
define void @writable_table(i64 %i) {
  %idx = and i64 %i, 3
  %p = getelementptr inbounds [8 x i8], ptr @writable, i64 %idx
  %f = load ptr, ptr %p
  call void %f() [ "kcfi"(i32 12) ]
  ret void
}

; CHECK-LABEL: define void @placed_table(
; CHECK:         call {{.*}}@__llvm_kcfi_{{dispatch|check}}_0000000c(
define void @placed_table(i64 %i) {
  %idx = and i64 %i, 3
  %p = getelementptr inbounds [8 x i8], ptr @placed, i64 %idx
  %f = load ptr, ptr %p
  call void %f() [ "kcfi"(i32 12) ]
  ret void
}

; CHECK-LABEL: define void @dllimport_entry(
; CHECK:         call {{.*}}@__llvm_kcfi_{{dispatch|check}}_0000000c(
define void @dllimport_entry(i64 %i) {
  %idx = and i64 %i, 3
  %p = getelementptr inbounds [8 x i8], ptr @imports, i64 %idx
  %f = load ptr, ptr %p
  call void %f() [ "kcfi"(i32 12) ]
  ret void
}

!llvm.module.flags = !{!0, !1, !2}
!0 = !{i32 4, !"kcfi", i32 1}
!1 = !{i32 4, !"kcfi-marker", i32 -559038737}
!2 = !{i32 2, !"cfguard", i32 2}

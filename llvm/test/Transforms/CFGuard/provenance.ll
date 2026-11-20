; DEFINE: %{check} = opt -S -passes=cfguard | FileCheck %s --implicit-check-not=cfguard_proven
; RUN: cat %s | %{check}
; RUN: sed 's/x86_64-unknown-windows-itanium/x86_64-pc-windows-ntposix/' %s | %{check}
; RUN: sed 's/x86_64-unknown-windows-itanium/aarch64-unknown-windows-itanium/' %s | %{check}
; RUN: sed 's/x86_64-unknown-windows-itanium/x86_64-pc-windows-msvc/' %s | %{check} --check-prefix=NONE

;; An indirect call whose target is loaded from a constant table of functions,
;; at an index proven in range without relying on inbounds or any other
;; undefined behaviour, can only reach one of the table's entries. On Windows
;; Itanium and NT-POSIX the load is marked, and the call keeps its guard, which
;; the backend drops if the target stays in its register until the call. The
;; promises the proof did not read are dropped from the index, so that the
;; backend cannot compute another one from them. Other triples mark nothing.
;; Every load not checked here is unmarked.

target triple = "x86_64-unknown-windows-itanium"

declare !kcfi_type !1 void @f()
declare !kcfi_type !1 void @g()
declare !kcfi_type !2 void @h()
declare void @u()
declare !kcfi_type !1 dllimport void @imp()
declare i8 @next8()
declare zeroext i8 @next8_extended()
declare range(i64 0, 4) i64 @next_in_range()
declare i64 @llvm.umin.i64(i64, i64)

@table = internal constant [4 x ptr] [ptr @f, ptr null, ptr @g, ptr @f]
@bytes = internal constant [256 x ptr] zeroinitializer
@writable = internal global [4 x ptr] [ptr @f, ptr @g, ptr @f, ptr @g]
@interposable = weak constant [4 x ptr] [ptr @f, ptr @g, ptr @f, ptr @g]
@placed = internal constant [4 x ptr] [ptr @f, ptr @g, ptr @f, ptr @g], section ".sealed"
@local = internal thread_local constant [4 x ptr] [ptr @f, ptr @g, ptr @f, ptr @g]
@imports = internal constant [4 x ptr] [ptr @f, ptr @imp, ptr @f, ptr @g]
@mixed = internal constant [4 x ptr] [ptr @f, ptr @h, ptr @f, ptr @g]
@untyped = internal constant [4 x ptr] [ptr @f, ptr @u, ptr @f, ptr @g]
@fields = internal constant { i64, [4 x ptr] } { i64 0, [4 x ptr] [ptr @f, ptr @g, ptr @f, ptr @g] }

;; A masked index selects one of the four entries, null or of the call's type. A
;; null entry faults when called, so it needs no check either. The call keeps
;; its guard.
; CHECK-LABEL: define void @mask(
; CHECK:       %f = load ptr, ptr %p, align 8, !cfguard_proven
; CHECK:       load ptr, ptr @__guard_{{dispatch|check}}_icall_fptr
; NONE:        load ptr, ptr @__guard_{{dispatch|check}}_icall_fptr
define void @mask(i64 %i) {
  %idx = and i64 %i, 3
  %p = getelementptr [8 x i8], ptr @table, i64 %idx
  %f = load ptr, ptr %p
  call void %f() [ "kcfi"(i32 12) ]
  ret void
}

;; Every value of a zero-extended i8 selects one of 256 entries. Arithmetic with
;; a constant bounds the index as a mask does. A call without a kcfi bundle
;; checks no type, so a table of any functions will do. With nuw or exact the
;; backend may fold shifts or a truncation away: the proof reads no such flag,
;; and drops it.
; CHECK-LABEL: define void @bounded(
; CHECK:       %f1 = load ptr, ptr %p1, align 8, !cfguard_proven
; CHECK:       %f2 = load ptr, ptr %p2, align 8, !cfguard_proven
; CHECK:       %f3 = load ptr, ptr %p3, align 8, !cfguard_proven
; CHECK:       %s = shl i64 %i, 1
; CHECK:       %e = lshr i64 %s, 1
; CHECK:       %t = trunc i64 %e to i8
; CHECK:       %f4 = load ptr, ptr %p4, align 8, !cfguard_proven
define void @bounded(i8 %op, i64 %i) {
  %x1 = zext i8 %op to i64
  %p1 = getelementptr [8 x i8], ptr @bytes, i64 %x1
  %f1 = load ptr, ptr %p1
  call void %f1() [ "kcfi"(i32 12) ]
  %hi = lshr i64 %i, 56
  %rem = urem i64 %hi, 3
  %x2 = add i64 %rem, 1
  %p2 = getelementptr [8 x i8], ptr @table, i64 %x2
  %f2 = load ptr, ptr %p2
  call void %f2() [ "kcfi"(i32 12) ]
  %x3 = and i64 %i, 3
  %p3 = getelementptr [8 x i8], ptr @mixed, i64 %x3
  %f3 = load ptr, ptr %p3
  call void %f3()
  %s = shl nuw i64 %i, 1
  %e = lshr exact i64 %s, 1
  %t = trunc nuw i64 %e to i8
  %x4 = zext i8 %t to i64
  %p4 = getelementptr [8 x i8], ptr @bytes, i64 %x4
  %f4 = load ptr, ptr %p4
  call void %f4() [ "kcfi"(i32 12) ]
  ret void
}

;; A compare on the only edge into the block bounds the index, on either
;; successor.
; CHECK-LABEL: define void @bounds_check(
; CHECK:       %f = load ptr, ptr %p, align 8, !cfguard_proven
define void @bounds_check(i32 %i) {
entry:
  %c = icmp ult i32 %i, 256
  br i1 %c, label %call, label %exit
call:
  %idx = zext i32 %i to i64
  %p = getelementptr [8 x i8], ptr @bytes, i64 %idx
  %f = load ptr, ptr %p
  call void %f() [ "kcfi"(i32 12) ]
  br label %exit
exit:
  ret void
}

; CHECK-LABEL: define void @bounds_check_false_edge(
; CHECK:       %f = load ptr, ptr %p, align 8, !cfguard_proven
define void @bounds_check_false_edge(i32 %i) {
entry:
  %c = icmp uge i32 %i, 256
  br i1 %c, label %exit, label %call
call:
  %idx = zext i32 %i to i64
  %p = getelementptr [8 x i8], ptr @bytes, i64 %idx
  %f = load ptr, ptr %p
  call void %f() [ "kcfi"(i32 12) ]
  br label %exit
exit:
  ret void
}

;; A callee may spill a value that is live across a call: the compared index,
;; or the address and the loaded target. The mark is on the load, so a second
;; call of the loaded value leaves it unmarked.
define void @call_after_compare(i32 %i) {
entry:
  %c = icmp ult i32 %i, 256
  call void @g()
  br i1 %c, label %call, label %exit
call:
  %idx = zext i32 %i to i64
  %p = getelementptr [8 x i8], ptr @bytes, i64 %idx
  %f = load ptr, ptr %p
  call void %f() [ "kcfi"(i32 12) ]
  br label %exit
exit:
  ret void
}

define void @call_after_edge(i32 %i) {
entry:
  %c = icmp ult i32 %i, 256
  br i1 %c, label %call, label %exit
call:
  call void @g()
  %idx = zext i32 %i to i64
  %p = getelementptr [8 x i8], ptr @bytes, i64 %idx
  %f = load ptr, ptr %p
  call void %f() [ "kcfi"(i32 12) ]
  br label %exit
exit:
  ret void
}

define void @hoisted_load(i64 %i, i1 %c) {
entry:
  %idx = and i64 %i, 3
  %p = getelementptr [8 x i8], ptr @table, i64 %idx
  %f = load ptr, ptr %p
  br label %loop
loop:
  call void %f() [ "kcfi"(i32 12) ]
  br i1 %c, label %loop, label %exit
exit:
  ret void
}

define void @call_after_load(i64 %i) {
  %idx = and i64 %i, 3
  %p = getelementptr [8 x i8], ptr @table, i64 %idx
  %f = load ptr, ptr %p
  call void @g()
  call void %f() [ "kcfi"(i32 12) ]
  %idx2 = and i64 %i, 3
  %p2 = getelementptr [8 x i8], ptr @table, i64 %idx2
  %f2 = load ptr, ptr %p2
  call void %f2() [ "kcfi"(i32 12) ]
  call void %f2() [ "kcfi"(i32 12) ]
  ret void
}

;; The backend may lower a samesign compare as a signed one and an nneg zext as
;; a sext, either of which a negative index passes. The proof drops both.
; CHECK-LABEL: define void @flags(
; CHECK:       %c = icmp ult i32 %i, 256
; CHECK:       %idx = zext i32 %i to i64
; CHECK:       %f = load ptr, ptr %p, align 8, !cfguard_proven
define void @flags(i32 %i) {
entry:
  %c = icmp samesign ult i32 %i, 256
  br i1 %c, label %call, label %exit
call:
  %idx = zext nneg i32 %i to i64
  %p = getelementptr [8 x i8], ptr @bytes, i64 %idx
  %f = load ptr, ptr %p
  call void %f() [ "kcfi"(i32 12) ]
  br label %exit
exit:
  ret void
}

;; A promise about a value the index is computed from would let the backend
;; delete the mask: a range on an argument or a load, a flag, or the same on a
;; value stored where the block loads it back. Without zeroext, the function
;; extends its argument itself.
; CHECK-LABEL: define void @roots(
; CHECK-SAME:  i8 %a, ptr %q, ptr %slot)
; CHECK:       %b = load i8, ptr %q, align 1{{$}}
; CHECK:       %sum = add i8 %a, %b
; CHECK:       %wide = zext i8 %sum to i64
; CHECK:       %f = load ptr, ptr %p, align 8, !cfguard_proven
define void @roots(i8 zeroext range(i8 0, 4) %a, ptr %q, ptr %slot) {
  %b = load i8, ptr %q, !range !3, !noundef !4
  %sum = add nuw i8 %a, %b
  %wide = zext nneg i8 %sum to i64
  store i64 %wide, ptr %slot
  %i = load i64, ptr %slot
  %idx = and i64 %i, 3
  %p = getelementptr [8 x i8], ptr @table, i64 %idx
  %f = load ptr, ptr %p
  call void %f() [ "kcfi"(i32 12) ]
  ret void
}

;; A call's result is what the callee returned. Without zeroext, the caller
;; extends it itself: a callee need not. What the callee's declaration
;; promises cannot be dropped, so it proves nothing.
; CHECK-LABEL: define void @call_root(
; CHECK:       %op = call i8 @next8()
; CHECK:       %f = load ptr, ptr %p, align 8, !cfguard_proven
define void @call_root() {
  %op = call zeroext range(i8 0, 4) i8 @next8()
  %idx = zext i8 %op to i64
  %p = getelementptr [8 x i8], ptr @bytes, i64 %idx
  %f = load ptr, ptr %p
  call void %f() [ "kcfi"(i32 12) ]
  %i = call i64 @next_in_range()
  %idx1 = and i64 %i, 3
  %p1 = getelementptr [8 x i8], ptr @table, i64 %idx1
  %f1 = load ptr, ptr %p1
  call void %f1() [ "kcfi"(i32 12) ]
  %op2 = call i8 @next8_extended()
  %idx2 = zext i8 %op2 to i64
  %p2 = getelementptr [8 x i8], ptr @bytes, i64 %idx2
  %f2 = load ptr, ptr %p2
  call void %f2() [ "kcfi"(i32 12) ]
  ret void
}

;; Indexes the proof does not bound, each computed after the call before it: a
;; zero-extended i16 or a sign-extended i8 into 256 entries; a remainder by zero,
;; a shift by the width or by a variable, which are poison for some operands; an
;; intrinsic; a chain deeper than the proof follows; inbounds, which only makes
;; an out-of-range address poison; and an offset that is not a whole entry.
define void @unbounded(i64 %i, i64 %n, i16 %h, i8 %b) {
  %x1 = zext i16 %h to i64
  %p1 = getelementptr [8 x i8], ptr @bytes, i64 %x1
  %f1 = load ptr, ptr %p1
  call void %f1() [ "kcfi"(i32 12) ]
  %x2 = sext i8 %b to i64
  %p2 = getelementptr [8 x i8], ptr @bytes, i64 %x2
  %f2 = load ptr, ptr %p2
  call void %f2() [ "kcfi"(i32 12) ]
  %x3 = urem i64 %i, 0
  %p3 = getelementptr [8 x i8], ptr @table, i64 %x3
  %f3 = load ptr, ptr %p3
  call void %f3() [ "kcfi"(i32 12) ]
  %x4 = lshr i64 %i, 64
  %p4 = getelementptr [8 x i8], ptr @table, i64 %x4
  %f4 = load ptr, ptr %p4
  call void %f4() [ "kcfi"(i32 12) ]
  %s5 = lshr i64 %i, %n
  %x5 = and i64 %s5, 3
  %p5 = getelementptr [8 x i8], ptr @table, i64 %x5
  %f5 = load ptr, ptr %p5
  call void %f5() [ "kcfi"(i32 12) ]
  %m6 = call i64 @llvm.umin.i64(i64 %i, i64 %n)
  %x6 = and i64 %m6, 3
  %p6 = getelementptr [8 x i8], ptr @table, i64 %x6
  %f6 = load ptr, ptr %p6
  call void %f6() [ "kcfi"(i32 12) ]
  %a7 = and i64 %i, 1
  %b7 = add i64 %a7, 0
  %c7 = add i64 %b7, 0
  %d7 = add i64 %c7, 0
  %e7 = add i64 %d7, 0
  %g7 = add i64 %e7, 0
  %x7 = add i64 %g7, 0
  %p7 = getelementptr [8 x i8], ptr @table, i64 %x7
  %f7 = load ptr, ptr %p7
  call void %f7() [ "kcfi"(i32 12) ]
  %p8 = getelementptr inbounds [4 x ptr], ptr @table, i64 0, i64 %i
  %f8 = load ptr, ptr %p8
  call void %f8() [ "kcfi"(i32 12) ]
  %x9 = and i64 %i, 3
  %p9 = getelementptr i8, ptr @table, i64 %x9
  %f9 = load ptr, ptr %p9
  call void %f9() [ "kcfi"(i32 12) ]
  ret void
}

;; Tables that prove nothing: one beside data that is no function; one that may
;; be written, replaced at link time or placed in a section of the user's
;; choosing; one that names a function of another image; and, for a call with
;; a kcfi bundle, one with an entry of another type or of none, which a check
;; of the call's type would reject. A call without a bundle checks no type.
define void @bad_table(i64 %i) {
  %x1 = and i64 %i, 3
  %p1 = getelementptr [8 x i8], ptr getelementptr inbounds ({ i64, [4 x ptr] }, ptr @fields, i64 0, i32 1), i64 %x1
  %f1 = load ptr, ptr %p1
  call void %f1() [ "kcfi"(i32 12) ]
  %x2 = and i64 %i, 3
  %p2 = getelementptr [8 x i8], ptr @writable, i64 %x2
  %f2 = load ptr, ptr %p2
  call void %f2() [ "kcfi"(i32 12) ]
  %x3 = and i64 %i, 3
  %p3 = getelementptr [8 x i8], ptr @interposable, i64 %x3
  %f3 = load ptr, ptr %p3
  call void %f3() [ "kcfi"(i32 12) ]
  %x4 = and i64 %i, 3
  %p4 = getelementptr [8 x i8], ptr @placed, i64 %x4
  %f4 = load ptr, ptr %p4
  call void %f4() [ "kcfi"(i32 12) ]
  %x5 = and i64 %i, 3
  %p5 = getelementptr [8 x i8], ptr @local, i64 %x5
  %f5 = load ptr, ptr %p5
  call void %f5() [ "kcfi"(i32 12) ]
  %x6 = and i64 %i, 3
  %p6 = getelementptr [8 x i8], ptr @imports, i64 %x6
  %f6 = load ptr, ptr %p6
  call void %f6() [ "kcfi"(i32 12) ]
  %x7 = and i64 %i, 3
  %p7 = getelementptr [8 x i8], ptr @mixed, i64 %x7
  %f7 = load ptr, ptr %p7
  call void %f7() [ "kcfi"(i32 12) ]
  %x8 = and i64 %i, 3
  %p8 = getelementptr [8 x i8], ptr @untyped, i64 %x8
  %f8 = load ptr, ptr %p8
  call void %f8() [ "kcfi"(i32 12) ]
  ret void
}

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"cfguard", i32 2}
!1 = !{i32 12}
!2 = !{i32 13}
!3 = !{i8 0, i8 4}
!4 = !{}

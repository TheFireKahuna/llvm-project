; RUN: opt -S -passes=globalsplit %s | FileCheck %s

; A pinned global is split like any other. Each pin moves, with its offset
; adjusted, to the piece that holds the address it pins, found as for type
; metadata: an address at a boundary between two pieces is the end of the
; first, as the address point of a vtable without virtual functions is. Each
; piece keeps the alignment its offset had in the global.

target datalayout = "e-p:64:64"
target triple = "x86_64-unknown-windows-itanium"

; A required pin of the global's start.
; CHECK: @global.0 = private constant [5 x ptr] {{.*}}, align 8, !type ![[T24A:[0-9]+]], !pin ![[P0:[0-9]+]]
; CHECK: @global.1 = private constant [4 x ptr] {{.*}}, align 8, !type ![[T24V:[0-9]+]]{{$}}
@global = internal constant { [5 x ptr], [4 x ptr] } {
  [5 x ptr] [ptr null, ptr null, ptr null, ptr @f, ptr @f],
  [4 x ptr] [ptr null, ptr inttoptr (i64 -8 to ptr), ptr null, ptr @f]
}, align 8, !type !0, !type !1, !pin !2

; A vtable group whose primary vtable has its address point 16 bytes into a
; 64-byte line, with the pin that says so.
; CHECK: @vt.0 = private constant [3 x ptr] {{.*}}, align 64, !type ![[T16A:[0-9]+]], !pin ![[P16:[0-9]+]]
; CHECK: @vt.1 = private constant [3 x ptr] {{.*}}, align 8, !type ![[T16B:[0-9]+]]{{$}}
@vt = internal constant { [3 x ptr], [3 x ptr] } {
  [3 x ptr] [ptr null, ptr null, ptr @f],
  [3 x ptr] [ptr inttoptr (i64 -8 to ptr), ptr null, ptr @f]
}, align 64, !type !3, !type !4, !pin !5

; Pins at the boundary between the pieces and at the end of the global.
; CHECK: @edge.0 = private constant [3 x ptr] {{.*}}, align 32, !type ![[T24C:[0-9]+]], !pin ![[P24:[0-9]+]]
; CHECK: @edge.1 = private constant [3 x ptr] {{.*}}, align 8, !type ![[T24D:[0-9]+]], !pin ![[P24R:[0-9]+]]
@edge = internal constant { [3 x ptr], [3 x ptr] } {
  [3 x ptr] [ptr null, ptr null, ptr null],
  [3 x ptr] [ptr inttoptr (i64 -24 to ptr), ptr null, ptr null]
}, align 32, !type !6, !type !7, !pin !8, !pin !9

; CHECK-NOT: @global =
; CHECK-NOT: @vt =
; CHECK-NOT: @edge =

@vtt = constant [6 x ptr] [
  ptr getelementptr inrange(-24, 16) ({ [5 x ptr], [4 x ptr] }, ptr @global, i32 0, i32 0, i32 3),
  ptr getelementptr inrange(-24, 8) ({ [5 x ptr], [4 x ptr] }, ptr @global, i32 0, i32 1, i32 3),
  ptr getelementptr inrange(-16, 8) ({ [3 x ptr], [3 x ptr] }, ptr @vt, i32 0, i32 0, i32 2),
  ptr getelementptr inrange(-16, 8) ({ [3 x ptr], [3 x ptr] }, ptr @vt, i32 0, i32 1, i32 2),
  ptr getelementptr inrange(-24, 0) ({ [3 x ptr], [3 x ptr] }, ptr @edge, i32 0, i32 1, i32 0),
  ptr getelementptr inrange(-24, 0) ({ [3 x ptr], [3 x ptr] }, ptr @edge, i32 1, i32 0, i32 0)
]

define ptr @f() {
  ret ptr null
}

define void @foo() {
  %p = call i1 @llvm.type.test(ptr null, metadata !"")
  ret void
}

declare i1 @llvm.type.test(ptr, metadata) nounwind readnone

; CHECK-DAG: ![[T24A]] = !{i32 24, !"_ZTS1A"}
; CHECK-DAG: ![[T24V]] = !{i32 24, !"_ZTS1V"}
; CHECK-DAG: ![[P0]] = !{i64 0, i64 12, i64 4064, i64 1}
; CHECK-DAG: ![[T16A]] = !{i32 16, !"_ZTS1B"}
; CHECK-DAG: ![[T16B]] = !{i32 16, !"_ZTS1C"}
; CHECK-DAG: ![[P16]] = !{i64 16, i64 6, i64 16, i64 0}
; CHECK-DAG: ![[T24C]] = !{i32 24, !"_ZTS1D"}
; CHECK-DAG: ![[T24D]] = !{i32 24, !"_ZTS1E"}
; CHECK-DAG: ![[P24]] = !{i64 24, i64 12, i64 56, i64 1}
; CHECK-DAG: ![[P24R]] = !{i64 24, i64 12, i64 80, i64 1}

!0 = !{i64 24, !"_ZTS1A"}
!1 = !{i64 64, !"_ZTS1V"}
!2 = !{i64 0, i64 12, i64 4064, i64 1}
!3 = !{i64 16, !"_ZTS1B"}
!4 = !{i64 40, !"_ZTS1C"}
!5 = !{i64 16, i64 6, i64 16, i64 0}
!6 = !{i64 24, !"_ZTS1D"}
!7 = !{i64 48, !"_ZTS1E"}
!8 = !{i64 24, i64 12, i64 56, i64 1}
!9 = !{i64 48, i64 12, i64 80, i64 1}

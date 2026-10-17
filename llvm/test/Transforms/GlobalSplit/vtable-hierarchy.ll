; RUN: opt -S -passes=globalsplit %s | FileCheck %s

; A vtable group's vtable_hierarchy metadata moves, with its offset adjusted,
; to the piece that holds its address point, found as for type metadata.

target datalayout = "e-p:64:64"
target triple = "x86_64-unknown-windows-itanium"

; CHECK: @vt.0 = private constant [3 x ptr] {{.*}}, align 64, !type ![[T16A:[0-9]+]], !vtable_hierarchy ![[H16:[0-9]+]]
; CHECK: @vt.1 = private constant [3 x ptr] {{.*}}, align 8, !type ![[T16B:[0-9]+]]{{$}}
@vt = internal constant { [3 x ptr], [3 x ptr] } {
  [3 x ptr] [ptr null, ptr null, ptr @f],
  [3 x ptr] [ptr inttoptr (i64 -8 to ptr), ptr null, ptr @f]
}, align 64, !type !0, !type !1, !vtable_hierarchy !2

; CHECK-NOT: @vt =

@vtt = constant [2 x ptr] [
  ptr getelementptr inrange(-16, 8) ({ [3 x ptr], [3 x ptr] }, ptr @vt, i32 0, i32 0, i32 2),
  ptr getelementptr inrange(-16, 8) ({ [3 x ptr], [3 x ptr] }, ptr @vt, i32 0, i32 1, i32 2)
]

define ptr @f() {
  ret ptr null
}

define void @foo() {
  %p = call i1 @llvm.type.test(ptr null, metadata !"")
  ret void
}

declare i1 @llvm.type.test(ptr, metadata) nounwind readnone

; CHECK-DAG: ![[T16A]] = !{i32 16, !"_ZTS1B"}
; CHECK-DAG: ![[T16B]] = !{i32 16, !"_ZTS1C"}
; CHECK-DAG: ![[H16]] = !{i64 16, !"1B", !"1A"}

!0 = !{i64 16, !"_ZTS1B"}
!1 = !{i64 40, !"_ZTS1C"}
!2 = !{i64 16, !"1B", !"1A"}

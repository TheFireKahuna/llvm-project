; RUN: opt -S -passes=globalsplit %s | FileCheck %s

; A pinned global is not split: the pin fixes the addresses of its parts
; relative to each other.

target datalayout = "e-p:64:64"
target triple = "x86_64-unknown-windows-itanium"

; CHECK: @global = internal constant { [5 x ptr], [4 x ptr] } {{.*}}, align 8, !type !0, !type !1, !pin !2
; CHECK-NOT: @global.0
@global = internal constant { [5 x ptr], [4 x ptr] } {
  [5 x ptr] [ptr null, ptr null, ptr null, ptr @f, ptr @f],
  [4 x ptr] [ptr null, ptr inttoptr (i64 -8 to ptr), ptr null, ptr @f]
}, align 8, !type !0, !type !1, !pin !2

@vtt = constant [2 x ptr] [
  ptr getelementptr inrange(-24, 16) ({ [5 x ptr], [4 x ptr] }, ptr @global, i32 0, i32 0, i32 3),
  ptr getelementptr inrange(-24, 8) ({ [5 x ptr], [4 x ptr] }, ptr @global, i32 0, i32 1, i32 3)
]

define ptr @f() {
  ret ptr null
}

define void @foo() {
  %p = call i1 @llvm.type.test(ptr null, metadata !"")
  ret void
}

declare i1 @llvm.type.test(ptr, metadata) nounwind readnone

!0 = !{i64 24, !"_ZTS1A"}
!1 = !{i64 64, !"_ZTS1V"}
!2 = !{i64 0, i64 12, i64 4064, i64 1}

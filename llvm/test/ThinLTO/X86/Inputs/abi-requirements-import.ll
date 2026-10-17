target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-windows-itanium"

@s = external global [2 x i32]
@other = external global i32

define i32 @f() {
entry:
  %p = getelementptr [2 x i32], ptr @s, i64 0, i64 1
  %v = load i32, ptr %p
  ret i32 %v
}

define i32 @h() {
entry:
  %v = load i32, ptr @other
  ret i32 %v
}

!abi.requirements = !{!0, !1}
!0 = !{ptr @s, !2}
!1 = !{ptr @other, !3}
!2 = !{!"cS"}
!3 = !{!"cO"}

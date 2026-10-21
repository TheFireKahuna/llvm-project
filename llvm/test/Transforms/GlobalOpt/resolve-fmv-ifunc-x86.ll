; REQUIRES: x86-registered-target

; RUN: opt --passes=globalopt -o - -S < %s | FileCheck %s

; A call to an x86 multiversioned function resolves to the version its caller's
; own features select: the caller's code runs only where they exist.

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

@__cpu_model = external dso_local global { i32, i32, i32, [1 x i32] }
@__cpu_features2 = external dso_local global [3 x i32]

@wide = weak_odr ifunc i32 (), ptr @wide.resolver
@level = weak_odr ifunc i32 (), ptr @level.resolver
@cpu = weak_odr ifunc i32 (), ptr @cpu.resolver

declare void @__cpu_indicator_init()

declare i32 @wide.avx512f() #0
declare i32 @wide.default() #1

define weak_odr ptr @wide.resolver() {
  call void @__cpu_indicator_init()
  %features = load i32, ptr getelementptr inbounds nuw (i8, ptr @__cpu_model, i64 12)
  %mask = and i32 %features, 32768
  %has = icmp eq i32 %mask, 32768
  %version = select i1 %has, ptr @wide.avx512f, ptr @wide.default
  ret ptr %version
}

declare i32 @level.x86-64-v4() #2
declare i32 @level.default() #1

define weak_odr ptr @level.resolver() {
  call void @__cpu_indicator_init()
  %features = load i32, ptr getelementptr inbounds nuw (i8, ptr @__cpu_features2, i64 8)
  %mask = and i32 %features, 4
  %has = icmp eq i32 %mask, 4
  %version = select i1 %has, ptr @level.x86-64-v4, ptr @level.default
  ret ptr %version
}

; A version chosen by the processor's identity carries no features, so no
; call to its function resolves statically.
declare i32 @cpu.znver4() #3
declare i32 @cpu.default() #1

define weak_odr ptr @cpu.resolver() {
  call void @__cpu_indicator_init()
  %type = load i32, ptr getelementptr inbounds nuw (i8, ptr @__cpu_model, i64 8)
  %is = icmp eq i32 %type, 29
  %version = select i1 %is, ptr @cpu.znver4, ptr @cpu.default
  ret ptr %version
}

define i32 @wide_caller() #4 {
; CHECK-LABEL: define i32 @wide_caller(
; CHECK:         call i32 @wide.avx512f()
  %r = call i32 @wide()
  ret i32 %r
}

define i32 @plain_caller() #5 {
; CHECK-LABEL: define i32 @plain_caller(
; CHECK:         call i32 @wide()
  %r = call i32 @wide()
  ret i32 %r
}

; Every feature of the x86-64-v4 level gives the caller the level.
define i32 @level_caller() #6 {
; CHECK-LABEL: define i32 @level_caller(
; CHECK:         call i32 @level.x86-64-v4()
  %r = call i32 @level()
  ret i32 %r
}

define i32 @partial_level_caller() #4 {
; CHECK-LABEL: define i32 @partial_level_caller(
; CHECK:         call i32 @level()
  %r = call i32 @level()
  ret i32 %r
}

define i32 @cpu_caller() #7 {
; CHECK-LABEL: define i32 @cpu_caller(
; CHECK:         call i32 @cpu()
  %r = call i32 @cpu()
  ret i32 %r
}

attributes #0 = { "fmv-features"="avx512f" "target-features"="+avx512f,+cx8,+mmx,+sse,+sse2,+x87" }
attributes #1 = { "fmv-features" "target-features"="+cx8,+mmx,+sse,+sse2,+x87" }
attributes #2 = { "fmv-features"="x86-64-v4" }
attributes #3 = { "target-cpu"="znver4" }
attributes #4 = { "target-features"="+avx512f,+cx8,+mmx,+sse,+sse2,+x87" }
attributes #5 = { "target-features"="+cx8,+mmx,+sse,+sse2,+x87" }
attributes #6 = { "target-features"="+avx2,+avx512bw,+avx512cd,+avx512dq,+avx512f,+avx512vl,+bmi,+bmi2,+cmov,+crc32,+cx16,+cx8,+f16c,+fma,+fxsr,+lzcnt,+mmx,+movbe,+popcnt,+sahf,+sse,+sse2,+sse4.2,+x87,+xsave" }
attributes #7 = { "target-cpu"="znver4" "target-features"="+avx512f,+cx8,+mmx,+sse,+sse2,+x87" }

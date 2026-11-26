; RUN: rm -rf %t && split-file %s %t
; RUN: not llc -mtriple=x86_64-unknown-windows-itanium < %t/raw.ll 2>&1 | FileCheck %s --check-prefix=RAW
; RUN: not llc -mtriple=x86_64-pc-windows-ntposix < %t/unmapped.ll 2>&1 | FileCheck %s --check-prefix=UNMAPPED
; RUN: not llc -mtriple=x86_64-unknown-windows-itanium < %t/tied.ll 2>&1 | FileCheck %s --check-prefix=TIED
; RUN: %if aarch64-registered-target %{ not llc -mtriple=aarch64-unknown-windows-itanium < %t/sve.ll 2>&1 | FileCheck %s --check-prefix=SVE %}

;; An ifunc whose body Windows cannot select as it maps the image needs a
;; resolver cell, which has no lowering yet, and the error says why: a raw
;; ifunc, a version that needs a feature Windows has no number for or does not
;; prove enabled, and two versions of one priority.

; RAW: LLVM ERROR: ifunc 'raw' needs a resolver cell, which is not supported on this platform: its resolver returns other than versions of a multiversioned function
; UNMAPPED: LLVM ERROR: ifunc 'unmapped' needs a resolver cell, which is not supported on this platform: version 'bw' needs 'avx512bw', which Windows selects no body by
; TIED: LLVM ERROR: ifunc 'tied' needs a resolver cell, which is not supported on this platform: versions 'a' and 'b' have one priority
; SVE: LLVM ERROR: ifunc 'sve' needs a resolver cell, which is not supported on this platform: version 'v' needs 'sve', which Windows selects no body by

;--- raw.ll
@raw = ifunc i32 (), ptr @resolver
define hidden ptr @resolver() {
  ret ptr @impl
}
define hidden i32 @impl() {
  ret i32 0
}

;--- unmapped.ll
@unmapped = ifunc i32 (), ptr @resolver
declare i1 @test()
define hidden ptr @resolver() {
  %a = call i1 @test()
  %x = select i1 %a, ptr @bw, ptr @default
  ret ptr %x
}
define hidden i32 @bw() "fmv-features"="avx512bw" {
  ret i32 1
}
define hidden i32 @default() "fmv-features" {
  ret i32 0
}

;--- tied.ll
@tied = ifunc i32 (), ptr @resolver
declare i1 @test()
define hidden ptr @resolver() {
  %a = call i1 @test()
  %b = call i1 @test()
  %x = select i1 %a, ptr @a, ptr @default
  %y = select i1 %b, ptr @b, ptr %x
  ret ptr %y
}
define hidden i32 @a() "fmv-features"="avx512f" {
  ret i32 2
}
define hidden i32 @b() "fmv-features"="avx512f" {
  ret i32 1
}
define hidden i32 @default() "fmv-features" {
  ret i32 0
}

;--- sve.ll
@sve = ifunc i32 (), ptr @resolver
declare i1 @test()
define hidden ptr @resolver() {
  %a = call i1 @test()
  %x = select i1 %a, ptr @v, ptr @default
  ret ptr %x
}
define hidden i32 @v() "fmv-features"="sve" {
  ret i32 1
}
define hidden i32 @default() "fmv-features" {
  ret i32 0
}

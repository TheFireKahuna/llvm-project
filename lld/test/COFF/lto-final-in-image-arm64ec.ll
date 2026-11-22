; REQUIRES: aarch64
;; ARM64EC binds through thunks, some made only after LTO, so LTO is told no
;; reference is final, even one to a definition in the image.

; RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
; RUN: llvm-as main.ll -o main.bc
; RUN: llc other.ll -filetype=obj -o other.obj
; RUN: llvm-mc -filetype=obj -triple=arm64ec-windows \
; RUN:   %S/Inputs/loadconfig-arm64ec.s -o loadconfig.obj
; RUN: lld-link -machine:arm64ec -dll -noentry -export:use -out:out.dll \
; RUN:   -lldsavetemps -lldemit:llvm main.bc other.obj loadconfig.obj
; RUN: FileCheck %s < out.dll.resolution.txt

; CHECK:      main.bc
; CHECK-NEXT: -r=main.bc,{{.*}}use,px{{$}}
; CHECK-NEXT: -r=main.bc,__imp_vnative,x{{$}}

;--- main.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-p:64:64-i32:32-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "arm64ec-pc-windows-msvc"

@vnative = external dllimport global i32

define i32 @use() {
  %v = load i32, ptr @vnative
  ret i32 %v
}

;--- other.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-p:64:64-i32:32-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "arm64ec-pc-windows-msvc"

@vnative = global i32 5

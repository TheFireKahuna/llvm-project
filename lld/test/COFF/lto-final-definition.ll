; REQUIRES: x86
;; A PE image has no symbol preemption, so a prevailing definition in the image
;; is final for LTO, which may resolve references to it directly. Because the
;; prevailing definition is turned back into an undefined symbol only after
;; every file has been added, a file added after the one that defines a symbol
;; still sees the definition, so its reference is final too.

; RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
; RUN: llvm-as a.ll -o a.bc
; RUN: llvm-as b.ll -o b.bc
; RUN: lld-link -dll -noentry -export:use -out:out.dll -lldsavetemps a.bc b.bc
; RUN: FileCheck %s < out.dll.resolution.txt

; CHECK:      a.bc
;; g is defined here, so it is prevailing and final.
; CHECK-NEXT: -r=a.bc,g,pl{{$}}
; CHECK:      b.bc
;; b is added after a, so a's definition of g is still visible and b's
;; reference to it is final.
; CHECK-DAG:  -r=b.bc,g,l{{$}}
; CHECK-DAG:  -r=b.bc,use,plx{{$}}

;--- a.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

@g = global i32 1

;--- b.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

@g = external global i32

define i32 @use() {
  %v = load i32, ptr @g
  ret i32 %v
}

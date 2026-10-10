; REQUIRES: x86
;; A PE image has no symbol preemption, so weak and selectany data defined in
;; the image are final for LTO like any other definition there, whether the
;; definition is in bitcode or in a regular object.

; RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
; RUN: llvm-as main.ll -o main.bc
; RUN: llc other.ll -filetype=obj -o other.obj
; RUN: lld-link -dll -noentry -export:use -out:out.dll -lldsavetemps main.bc \
; RUN:   other.obj
; RUN: FileCheck %s < out.dll.resolution.txt

; CHECK:      main.bc
; CHECK-NEXT: -r=main.bc,use,plx{{$}}
; CHECK-NEXT: -r=main.bc,weak,pl{{$}}
; CHECK-NEXT: -r=main.bc,inline,lx{{$}}

;--- main.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-windows-itanium"

@weak = weak global i32 1
@inline = external global i32

define i32 @use() {
  %a = load i32, ptr @weak
  %b = load i32, ptr @inline
  %s = add i32 %a, %b
  ret i32 %s
}

;--- other.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-windows-itanium"

$inline = comdat any
@inline = linkonce_odr global i32 2, comdat
@keep = global ptr @inline

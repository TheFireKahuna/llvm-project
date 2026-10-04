; REQUIRES: aarch64
;; A dllimport reference to a definition in the image is final for LTO, so
;; LTO emits it as a direct reference. ARM64EC decides no finality.

; RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
; RUN: llvm-as main.ll -o main.bc
; RUN: llc other.ll -filetype=obj -o other.obj
; RUN: lld-link -machine:arm64 -dll -noentry -export:use -out:out.dll \
; RUN:   -lldsavetemps main.bc other.obj
; RUN: FileCheck --check-prefix=RES %s < out.dll.resolution.txt
; RUN: llvm-objdump -d --no-show-raw-insn out.dll | FileCheck %s

; RES:      main.bc
; RES-NEXT: -r=main.bc,__imp_innative,lx{{$}}
; RES-NEXT: -r=main.bc,use,plx{{$}}
; RES-NEXT: -r=main.bc,__imp_vnative,lx{{$}}

; CHECK-LABEL: <use>:
; CHECK-NOT:   blr
; CHECK:       bl 0x{{[0-9a-f]+}} <.text>
; CHECK-NEXT:  adrp x8, 0x{{[0-9a-f]+}}
; CHECK-NEXT:  ldr w8, [x8]
; CHECK-NOT:   blr

; RUN: llvm-as ec.ll -o ec.bc
; RUN: llc ec-other.ll -filetype=obj -o ec-other.obj
; RUN: llvm-mc -filetype=obj -triple=arm64ec-windows \
; RUN:   %S/Inputs/loadconfig-arm64ec.s -o loadconfig.obj
; RUN: lld-link -machine:arm64ec -dll -noentry -export:use -out:ec.dll \
; RUN:   -lldsavetemps -lldemit:llvm ec.bc ec-other.obj loadconfig.obj
; RUN: FileCheck --check-prefix=EC %s < ec.dll.resolution.txt

; EC:      ec.bc
; EC-NEXT: -r=ec.bc,{{.*}}use,px{{$}}
; EC-NEXT: -r=ec.bc,__imp_vnative,x{{$}}

;--- main.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-p:64:64-i32:32-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "aarch64-pc-windows-msvc"

declare dllimport i32 @innative()
@vnative = external dllimport global i32

define i32 @use() {
  %a = call i32 @innative()
  %b = load i32, ptr @vnative
  %s = add i32 %a, %b
  ret i32 %s
}

;--- other.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-p:64:64-i32:32-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "aarch64-pc-windows-msvc"

define i32 @innative() {
  ret i32 3
}

@vnative = global i32 5

;--- ec.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-p:64:64-i32:32-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "arm64ec-pc-windows-msvc"

@vnative = external dllimport global i32

define i32 @use() {
  %b = load i32, ptr @vnative
  ret i32 %b
}

;--- ec-other.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-p:64:64-i32:32-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "arm64ec-pc-windows-msvc"

@vnative = global i32 5

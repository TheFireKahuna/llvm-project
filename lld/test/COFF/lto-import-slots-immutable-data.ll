; REQUIRES: x86
;; Under -import-slots, a COMDAT of data that a DLL offers whole, which only a
;; constant initializes, with no guard and no initializer, stays prevailing
;; through LTO, though not final, so that the native link preempts it and
;; keeps its copy for the loads that only read values through its address.

; RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
; RUN: llvm-as main.ll -o main.bc
; RUN: lld-link -def:lib.def -out:lib.lib -machine:x64
; RUN: lld-link -import-slots -entry:main -subsystem:console -lldsavetemps \
; RUN:   main.bc lib.lib -out:out.exe -verbose 2>&1 | \
; RUN:   FileCheck --check-prefix=LOG %s
; RUN: FileCheck %s < out.exe.resolution.txt
; RUN: llvm-objdump -d out.exe | FileCheck --check-prefix=CODE %s
; RUN: llvm-readobj --coff-imports out.exe | \
; RUN:   FileCheck --check-prefix=IMPORTS %s

; LOG-DAG: Keeping the copy of tbl in out.exe.lto.obj for 1 value read
; LOG-DAG: Preempting tbl in out.exe.lto.obj with its instance in lib.dll

; CHECK: -r=main.bc,tbl,px{{$}}

; CODE:      <.text>:
; CODE-NEXT: 48 8d 05 {{.*}} leaq
; CODE-NEXT: 8b 04 88 movl (%rax,%rcx,4), %eax

; IMPORTS-NOT: lib.dll

;--- lib.def
LIBRARY lib.dll
EXPORTS
  tbl DATA

;--- main.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-windows-itanium"

$tbl = comdat any
@tbl = linkonce_odr constant [4 x i32] [i32 1, i32 2, i32 3, i32 4], comdat

define i32 @main(i64 %i) {
  %p = getelementptr inbounds [4 x i32], ptr @tbl, i64 0, i64 %i
  %v = load i32, ptr %p
  ret i32 %v
}

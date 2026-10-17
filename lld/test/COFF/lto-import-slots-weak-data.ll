; REQUIRES: x86
;; Under -import-slots, a COMDAT of data that a DLL the image imports from
;; offers whole is preempted before LTO, as the native link preempts it after:
;; its definitions are not prevailing, so LTO compiles no copy, and the code it
;; generates reaches them through their imports. Data that no DLL offers
;; stays final in the image, and a DLL, which offers it in turn, keeps it.

; RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
; RUN: llvm-as main.ll -o main.bc
; RUN: lld-link -def:lib.def -out:lib.lib -machine:x64
; RUN: lld-link -import-slots -dll -noentry -export:use -out:out.dll \
; RUN:   -lldsavetemps main.bc lib.lib -verbose 2>&1 | \
; RUN:   FileCheck --check-prefix=LOG %s
; RUN: FileCheck %s < out.dll.resolution.txt
; RUN: llvm-objdump -d out.dll | FileCheck --check-prefix=CODE %s
; RUN: llvm-readobj --coff-imports out.dll | FileCheck --check-prefix=IMPORTS %s

; LOG-DAG: Preempting v in main.bc with its instance in lib.dll
; LOG-DAG: Preempting guard variable for v in main.bc with its instance in lib.dll

; CHECK:      -r=main.bc,use,plx{{$}}
; CHECK-NEXT: -r=main.bc,v,{{$}}
; CHECK-NEXT: -r=main.bc,_ZGV1v,{{$}}
; CHECK-NEXT: -r=main.bc,local,plx{{$}}

; CODE:      <use>:
; CODE-NEXT: 48 8b 05 {{.*}} movq
; CODE-NEXT: 8b 00 movl (%rax), %eax
; CODE-NEXT: 48 8b 0d {{.*}} movq

; IMPORTS:      Name: lib.dll
; IMPORTS:      Symbol: _ZGV1v
; IMPORTS-NEXT: Symbol: v
; IMPORTS-NEXT: }

;--- lib.def
LIBRARY lib.dll
EXPORTS
  v DATA
  _ZGV1v DATA

;--- main.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-windows-itanium"

$v = comdat any
$_ZGV1v = comdat any
$local = comdat any
@v = linkonce_odr global i32 1, comdat
@_ZGV1v = linkonce_odr global i64 0, comdat
@local = linkonce_odr global i32 2, comdat

define i32 @use() {
  %a = load i32, ptr @v
  %g = load i64, ptr @_ZGV1v
  %b = load i32, ptr @local
  %t = trunc i64 %g to i32
  %s = add i32 %a, %b
  %r = add i32 %s, %t
  ret i32 %r
}

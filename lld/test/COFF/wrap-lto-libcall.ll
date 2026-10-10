; REQUIRES: x86

;; The LTO code generator lowers llvm.memset to a call to memset after the
;; wraps are decided, while no bitcode symbol names memset. A wrap of a runtime
;; library function therefore applies whenever there is bitcode, and that call
;; goes to __wrap_memset. A wrap of another unreferenced name is still ignored.

; RUN: rm -rf %t && split-file %s %t && cd %t
; RUN: llvm-as main.ll -o main.obj
; RUN: llvm-lib -machine:x64 -def:crt.def -out:crt.lib
; RUN: llvm-lib -machine:x64 -def:rt.def -out:rt.lib
; RUN: lld-link -entry:start -subsystem:console -out:main.exe main.obj \
; RUN:   crt.lib rt.lib -wrap:memset -wrap:foo
; RUN: llvm-readobj --coff-imports main.exe | FileCheck %s

; CHECK-NOT:  Name: crt.dll
; CHECK:      Name: rt.dll
; CHECK-NEXT: ImportLookupTableRVA:
; CHECK-NEXT: ImportAddressTableRVA:
; CHECK-NEXT: Symbol: __wrap_memset (0)
; CHECK-NEXT: }
; CHECK-NOT:  Name: crt.dll

;--- crt.def
LIBRARY crt.dll
EXPORTS
  memset
  foo

;--- rt.def
LIBRARY rt.dll
EXPORTS
  __wrap_memset
  __wrap_foo

;--- main.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

define void @start(ptr %p, i64 %n) {
  call void @llvm.memset.p0.i64(ptr %p, i8 0, i64 %n, i1 false)
  ret void
}

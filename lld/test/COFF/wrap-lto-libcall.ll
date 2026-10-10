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

;; A wrap of a runtime library function that code generation did not call
;; leaves no import of the wrapper, without garbage collection too, unless an
;; object calls the wrapper itself.
; RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc call.s -o call.obj
; RUN: lld-link -entry:start -subsystem:console -out:noref.exe main.obj \
; RUN:   crt.lib rt.lib -wrap:memset -wrap:memcpy -opt:noref
; RUN: llvm-readobj --coff-imports noref.exe | FileCheck %s
; RUN: lld-link -entry:start -subsystem:console -out:call.exe main.obj \
; RUN:   call.obj crt.lib rt.lib -wrap:memset -wrap:memcpy -opt:noref
; RUN: llvm-readobj --coff-imports call.exe | FileCheck --check-prefix=CALL %s

; CALL:     Name: rt.dll
; CALL-DAG: Symbol: __wrap_memset (0)
; CALL-DAG: Symbol: __wrap_memcpy (0)

;--- call.s
  .globl helper
helper:
  jmp __wrap_memcpy

;--- crt.def
LIBRARY crt.dll
EXPORTS
  memset
  memcpy
  foo

;--- rt.def
LIBRARY rt.dll
EXPORTS
  __wrap_memset
  __wrap_memcpy
  __wrap_foo

;--- main.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

define void @start(ptr %p, i64 %n) {
  call void @llvm.memset.p0.i64(ptr %p, i8 0, i64 %n, i1 false)
  ret void
}

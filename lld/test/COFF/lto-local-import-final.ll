; REQUIRES: x86

; A dllimport reference whose definition is in the link is bound directly by
; LTO: the definition is final, so the import indirection is dropped before
; code generation. A reference to another image's export keeps it.

; RUN: rm -rf %t.dir
; RUN: split-file %s %t.dir
; RUN: llvm-as %t.dir/main.ll -o %t.main.bc
; RUN: llvm-as %t.dir/other.ll -o %t.other.bc
; RUN: opt -module-summary %t.dir/main.ll -o %t.main.thin.bc
; RUN: opt -module-summary %t.dir/other.ll -o %t.other.thin.bc
; RUN: llc %t.dir/other.ll -o %t.other.obj --filetype=obj
; RUN: llvm-lib -out:%t.other.lib %t.other.obj
; RUN: llc %t.dir/dll.ll -o %t.dll.obj --filetype=obj
; RUN: lld-link -dll -noentry -out:%t.dll -implib:%t.dll.lib %t.dll.obj

;; Both in bitcode.
; RUN: lld-link -lldemit:asm -opt:lldlto=0 -entry:main -out:%t1 %t.main.bc %t.other.bc
; RUN: FileCheck %s --check-prefix=DIRECT < %t1.lto.s

;; Definition in a regular object, then in an archive member.
; RUN: lld-link -lldemit:asm -opt:lldlto=0 -entry:main -out:%t2 %t.main.bc %t.other.obj
; RUN: FileCheck %s --check-prefix=DIRECT < %t2.lto.s
; RUN: lld-link -lldemit:asm -opt:lldlto=0 -entry:main -out:%t3 %t.main.bc %t.other.lib
; RUN: FileCheck %s --check-prefix=DIRECT < %t3.lto.s

;; ThinLTO.
; RUN: lld-link -lldemit:asm -opt:lldlto=0 -entry:main -out:%t4 %t.main.thin.bc %t.other.thin.bc
; RUN: FileCheck %s --check-prefix=DIRECT < %t4.lto.%basename_t.tmp.main.thin.s

; DIRECT-LABEL: main:
; DIRECT-NOT: __imp_
; DIRECT: callq foo
; DIRECT: variable(%rip)
; DIRECT-NOT: __imp_
; DIRECT: retq

;; The definitions live in a DLL: the references stay indirect.
; RUN: lld-link -lldemit:asm -opt:lldlto=0 -entry:main -out:%t5 %t.main.bc %t.dll.lib
; RUN: FileCheck %s --check-prefix=IMPORT < %t5.lto.s

; IMPORT-LABEL: main:
; IMPORT: callq *__imp_foo(%rip)
; IMPORT: __imp_variable(%rip)

;--- main.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

define i32 @main() {
  %call = call i32 @foo()
  %v = load i32, ptr @variable
  %r = add i32 %v, %call
  ret i32 %r
}

@variable = external dllimport global i32
declare dllimport i32 @foo()

;--- other.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

define i32 @foo() noinline {
  ret i32 42
}

@variable = global i32 1

;--- dll.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

define dllexport i32 @foo() {
  ret i32 42
}

@variable = dllexport global i32 1

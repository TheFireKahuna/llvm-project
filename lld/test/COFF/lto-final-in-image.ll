; REQUIRES: x86
;; A reference that binds to a definition in the image is final for LTO, so
;; LTO emits it as a direct reference. A dllimport reference to such a
;; definition is final too; one to an import, to an absolute symbol or through
;; a defined __imp_ pointer is not.

; RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
; RUN: llvm-as main.ll -o main.bc
; RUN: llvm-as early.ll -o early.bc
; RUN: llc other.ll -filetype=obj -o other.obj
; RUN: llvm-mc -triple=x86_64-windows-msvc -filetype=obj abs.s -o abs.obj
; RUN: lld-link -def:imp.def -out:imp.lib -machine:x64

;; early.bc comes first, so its definitions prevail before main.bc is added.
; RUN: lld-link -dll -noentry -export:use -out:out.dll -lldsavetemps early.bc main.bc \
; RUN:   other.obj abs.obj imp.lib
; RUN: FileCheck --check-prefix=RES %s < out.dll.resolution.txt
; RUN: llvm-objdump -d --no-show-raw-insn out.dll | FileCheck %s

; RES:      early.bc
; RES-NEXT: -r=early.bc,inbitcode,plx{{$}}
; RES-NEXT: -r=early.bc,__imp_redirect,plx{{$}}
; RES:      main.bc
; RES-NEXT: -r=main.bc,__imp_innative,lx{{$}}
; RES-NEXT: -r=main.bc,__imp_inbitcode,lx{{$}}
; RES-NEXT: -r=main.bc,__imp_imported,x{{$}}
; RES-NEXT: -r=main.bc,__imp_redirect,x{{$}}
; RES-NEXT: -r=main.bc,use,plx{{$}}
; RES-NEXT: -r=main.bc,__imp_vnative,lx{{$}}
; RES-NEXT: -r=main.bc,plain,lx{{$}}
; RES-NEXT: -r=main.bc,absolute,x{{$}}

;; innative and vnative are direct, inbitcode's result is propagated into
;; use, and imported and redirect are called through their pointers.
; CHECK-LABEL: <use>:
; CHECK-NOT:   addr32
; CHECK:       callq 0x{{[0-9a-f]+}} <.text>
; CHECK-NEXT:  movl %eax, %esi
; CHECK-NEXT:  addl 0x{{[0-9a-f]+}}(%rip), %esi
; CHECK-NEXT:  callq *0x{{[0-9a-f]+}}(%rip)
; CHECK-NEXT:  movl %eax, %edi
; CHECK-NEXT:  callq *0x{{[0-9a-f]+}}(%rip)
; CHECK-NEXT:  addl %edi, %eax
; CHECK-NEXT:  addl %esi, %eax
; CHECK-NEXT:  addl 0x{{[0-9a-f]+}}(%rip), %eax
; CHECK-NOT:   callq

;--- main.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

declare dllimport i32 @innative()
@vnative = external dllimport global i32
declare dllimport i32 @inbitcode()
declare dllimport i32 @imported()
declare dllimport i32 @redirect()
@plain = external global i32
@absolute = external global i8

define i32 @use() {
  %a = call i32 @innative()
  %b = load i32, ptr @vnative
  %c = call i32 @imported()
  %d = call i32 @redirect()
  %e = load i32, ptr @plain
  %f = call i32 @inbitcode()
  %g = ptrtoint ptr @absolute to i32
  %s1 = add i32 %a, %b
  %s2 = add i32 %s1, %c
  %s3 = add i32 %s2, %d
  %s4 = add i32 %s3, %e
  %s5 = add i32 %s4, %f
  %s6 = add i32 %s5, %g
  ret i32 %s6
}

;--- early.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

define i32 @inbitcode() noinline {
  ret i32 1
}

@__imp_redirect = global ptr @replacement

define internal i32 @replacement() {
  ret i32 2
}

;--- other.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

define i32 @innative() {
  ret i32 3
}

define i32 @redirect() {
  ret i32 4
}

@vnative = global i32 5
@plain = global i32 6

;--- abs.s
.globl absolute
absolute = 0x1000

;--- imp.def
LIBRARY imp.dll
EXPORTS
  imported

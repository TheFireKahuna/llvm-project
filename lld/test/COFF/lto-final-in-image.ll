; REQUIRES: x86
;; A reference that binds to a definition in the image, directly or through a
;; weak alias, is final for LTO, so LTO emits it as a direct reference. A
;; dllimport reference to such a definition is final too; one to an import, to
;; an absolute symbol, or through an __imp_ pointer that is defined or has a
;; weak alias is not, and nor is a defined __imp_ pointer itself.

; RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
; RUN: llvm-as main.ll -o main.bc
; RUN: llvm-as early.ll -o early.bc
; RUN: llc other.ll -filetype=obj -o other.obj
; RUN: llvm-mc -triple=x86_64-windows-msvc -filetype=obj abs.s -o abs.obj
; RUN: llvm-mc -triple=x86_64-windows-msvc -filetype=obj weak.s -o weak.obj
; RUN: lld-link -def:imp.def -out:imp.lib -machine:x64

;; early.bc comes first, so its definitions prevail before main.bc is added.
; RUN: lld-link -dll -noentry -export:use -out:out.dll -lldsavetemps early.bc main.bc \
; RUN:   other.obj abs.obj weak.obj imp.lib \
; RUN:   -alternatename:__imp_aliased=__imp_imported
; RUN: FileCheck %s < out.dll.resolution.txt

; CHECK:      early.bc
; CHECK-NEXT: -r=early.bc,inbitcode,plx{{$}}
; CHECK-NEXT: -r=early.bc,__imp_redirect,px{{$}}
; CHECK:      main.bc
; CHECK-NEXT: -r=main.bc,__imp_innative,lx{{$}}
; CHECK-NEXT: -r=main.bc,__imp_inbitcode,lx{{$}}
; CHECK-NEXT: -r=main.bc,__imp_imported,x{{$}}
; CHECK-NEXT: -r=main.bc,__imp_redirect,x{{$}}
; CHECK-NEXT: -r=main.bc,__imp_viaweak,lx{{$}}
; CHECK-NEXT: -r=main.bc,__imp_aliased,x{{$}}
; CHECK-NEXT: -r=main.bc,use,plx{{$}}
; CHECK-NEXT: -r=main.bc,__imp_vnative,lx{{$}}
; CHECK-NEXT: -r=main.bc,plain,lx{{$}}
; CHECK-NEXT: -r=main.bc,absolute,x{{$}}

;; ThinLTO's backend emits such a dllimport reference directly too.
; RUN: opt -module-summary main.ll -o thin.bc
; RUN: opt -module-summary early.ll -o thin-early.bc
; RUN: lld-link -dll -noentry -export:use -out:thin.dll thin-early.bc thin.bc \
; RUN:   other.obj abs.obj weak.obj imp.lib -alternatename:__imp_aliased=__imp_imported
; RUN: llvm-objdump -d --no-show-raw-insn thin.dll | FileCheck --check-prefix=THIN %s
; THIN-LABEL: <use>:
; THIN:       callq 0x{{[0-9a-f]+}} <.text>
; THIN:       addl 0x{{[0-9a-f]+}}(%rip), %esi

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
declare dllimport i32 @viaweak()
declare dllimport i32 @aliased()

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
  %h = call i32 @viaweak()
  %i = call i32 @aliased()
  %s7 = add i32 %s6, %h
  %s8 = add i32 %s7, %i
  ret i32 %s8
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

define i32 @aliased() {
  ret i32 7
}

@vnative = global i32 5
@plain = global i32 6

;--- abs.s
.globl absolute
absolute = 0x1000

;--- weak.s
.weak viaweak
viaweak:
  ret

;--- imp.def
LIBRARY imp.dll
EXPORTS
  imported

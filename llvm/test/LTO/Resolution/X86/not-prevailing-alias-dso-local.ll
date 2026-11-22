;; The declaration that stands for an alias that does not prevail is as local
;; as the resolution makes the alias.
; RUN: llvm-as -o %t.o %s
; RUN: llvm-lto2 run -o %t2.o %t.o -r=%t.o,f,plx -r=%t.o,a,l -r=%t.o,h,l \
; RUN:   -r=%t.o,use,plx -save-temps
; RUN: llvm-dis -o - %t2.o.0.0.preopt.bc | FileCheck %s

; CHECK: declare dso_local void @a()
; CHECK: declare hidden void @h()

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

@a = linkonce_odr alias void (), ptr @f
@h = linkonce_odr hidden alias void (), ptr @f

define void @f() {
  ret void
}

define void @use() {
  call void @a()
  call void @h()
  ret void
}

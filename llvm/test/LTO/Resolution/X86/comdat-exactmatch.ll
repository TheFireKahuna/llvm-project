; The linker compares the copies of a COMDAT that selects by exact match, so
; LTO keeps one for it to compare. Regular LTO compares the copies its inputs
; hold, and the first stays a definition however the linker resolved it;
; ThinLTO keeps a copy that does not prevail.

; RUN: rm -rf %t && split-file %s %t && cd %t
; RUN: llvm-as a.ll -o a.bc
; RUN: llvm-as b.ll -o b.bc
; RUN: llvm-as same.ll -o same.bc

; RUN: not llvm-lto2 run -o out a.bc b.bc -r a.bc,c,px -r a.bc,a,px \
; RUN:   -r b.bc,c, -r b.bc,b,px 2>&1 | FileCheck --check-prefix=DIFF %s
; DIFF: COMDAT 'c' differs between a.bc and b.bc

; RUN: llvm-lto2 run -save-temps -o same a.bc same.bc -r a.bc,c, -r a.bc,a,px \
; RUN:   -r same.bc,c, -r same.bc,b,px
; RUN: llvm-dis same.0.0.preopt.bc -o - | FileCheck --check-prefix=KEEP %s
; KEEP: $c = comdat exactmatch
; KEEP: @c = weak_odr constant [2 x i8] c"a\00", comdat

; RUN: opt -module-summary a.ll -o a.thin.bc
; RUN: opt -module-summary same.ll -o same.thin.bc
; RUN: llvm-lto2 run -save-temps -o thin a.thin.bc same.thin.bc \
; RUN:   -r a.thin.bc,c,px -r a.thin.bc,a,px -r same.thin.bc,c, \
; RUN:   -r same.thin.bc,b,px
; RUN: llvm-dis thin.2.3.import.bc -o - | FileCheck --check-prefix=THIN %s
; THIN: @c = weak_odr constant [2 x i8] c"a\00", comdat

;--- a.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

$c = comdat exactmatch
@c = linkonce_odr constant [2 x i8] c"a\00", comdat
@a = global ptr @c

;--- b.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

$c = comdat exactmatch
@c = linkonce_odr constant [2 x i8] c"b\00", comdat
@b = global ptr @c

;--- same.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

$c = comdat exactmatch
@c = linkonce_odr constant [2 x i8] c"a\00", comdat
@b = global ptr @c

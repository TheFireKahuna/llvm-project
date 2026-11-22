; REQUIRES: x86

;; The copies of a COMDAT that selects by exact match are compared whichever
;; of them LTO compiles: a native copy that meets a bitcode leader is compared
;; with what LTO made of the leader, and LTO keeps a bitcode copy that a
;; native one prevails over, so that the link compares the two.

; RUN: rm -rf %t && split-file %s %t && cd %t
; RUN: llc -filetype=obj main.ll -o main.obj
; RUN: llc -filetype=obj a.ll -o a.obj
; RUN: llc -filetype=obj b.ll -o b.obj
; RUN: llvm-as a.ll -o a.bc
; RUN: llvm-as b.ll -o b.bc
; RUN: opt -module-summary a.ll -o a.thin.bc
; RUN: opt -module-summary b.ll -o b.thin.bc
; RUN: llc -filetype=obj same.ll -o same.obj
; RUN: llvm-as same.ll -o same.bc

; RUN: not lld-link -out:x.exe -subsystem:console main.obj a.bc b.obj 2>&1 \
; RUN:   | FileCheck --check-prefix=DIFF %s
; RUN: not lld-link -out:x.exe -subsystem:console main.obj a.obj b.bc 2>&1 \
; RUN:   | FileCheck --check-prefix=DIFF %s
; RUN: not lld-link -out:x.exe -subsystem:console main.obj a.thin.bc b.obj 2>&1 \
; RUN:   | FileCheck --check-prefix=DIFF %s
; RUN: not lld-link -out:x.exe -subsystem:console main.obj a.obj b.thin.bc 2>&1 \
; RUN:   | FileCheck --check-prefix=DIFF %s
; RUN: not lld-link -out:x.exe -subsystem:console main.obj a.thin.bc b.thin.bc 2>&1 \
; RUN:   | FileCheck --check-prefix=DIFF %s
; DIFF: duplicate symbol: c

; RUN: lld-link -out:x.exe -subsystem:console main.obj a.bc same.obj
; RUN: lld-link -out:x.exe -subsystem:console main.obj a.obj same.bc

;--- main.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

@refs = global [2 x ptr] [ptr @a, ptr @b]
@a = external global ptr
@b = external global ptr

define void @mainCRTStartup() {
  ret void
}

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

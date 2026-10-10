; REQUIRES: x86
; RUN: rm -rf %t && split-file %s %t

;; A function that __declspec(guard(suppress)) marks, "guard_suppress" in IR,
;; reaches the image listed as a call target but suppressed, whether it is
;; compiled to an object or under LTO, where code generation names it in the
;; directives of LTO's output, and whether it is external or local to its
;; object. Identical code folding leaves it out, so that a function it would
;; fold with stays a valid target.

; RUN: llvm-mc -triple x86_64-windows-msvc -filetype=obj %t/ldcfg.s \
; RUN:   -o %t/ldcfg.obj
; RUN: llc -filetype=obj -function-sections %t/main.ll -o %t/main.obj
; RUN: lld-link -entry:main -guard:cf -opt:ref,icf -debug:symtab %t/main.obj \
; RUN:   %t/ldcfg.obj -out:%t/obj.exe
; RUN: llvm-nm %t/obj.exe > %t/obj.txt
; RUN: llvm-readobj --coff-load-config %t/obj.exe >> %t/obj.txt
; RUN: FileCheck %s --input-file=%t/obj.txt
; RUN: llvm-as %t/main.ll -o %t/main.bc
; RUN: lld-link -entry:main -guard:cf -opt:ref,icf -debug:symtab %t/main.bc \
; RUN:   %t/ldcfg.obj -out:%t/lto.exe
; RUN: llvm-nm %t/lto.exe > %t/lto.txt
; RUN: llvm-readobj --coff-load-config %t/lto.exe >> %t/lto.txt
; RUN: FileCheck %s --input-file=%t/lto.txt

; CHECK-DAG: [[#%x,LOCAL:]] {{[tT]}} local
; CHECK-DAG: [[#%x,MAIN:]] {{[tT]}} main
; CHECK-DAG: [[#%x,PLAIN:]] {{[tT]}} plain
; CHECK-DAG: [[#%x,SUPPRESSED:]] {{[tT]}} suppressed
; CHECK:     GuardFidTable [
; CHECK-DAG:   0x[[#%X,SUPPRESSED]] flags 1
; CHECK-DAG:   0x[[#%X,LOCAL]] flags 1
; CHECK-DAG:   0x[[#%X,PLAIN]]{{$}}
; CHECK-DAG:   0x[[#%X,MAIN]]{{$}}
; CHECK:     ]

;--- main.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

@targets = dso_local global [3 x ptr] [ptr @suppressed, ptr @local, ptr @plain]

define dso_local void @suppressed() #0 {
  ret void
}

define internal void @local() #0 {
  ret void
}

define dso_local void @plain() {
  ret void
}

define dso_local i32 @main() {
  %f = load volatile ptr, ptr @targets
  ret i32 0
}

attributes #0 = { noinline "guard_suppress" }

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"cfguard", i32 2}

;--- ldcfg.s
        .section .rdata,"dr"
        .p2align 3
        .globl _load_config_used
_load_config_used:
        .long 256
        .fill 124, 1, 0
        .quad __guard_fids_table
        .quad __guard_fids_count
        .long __guard_flags
        .fill 12, 1, 0
        .quad __guard_iat_table
        .quad __guard_iat_count
        .quad __guard_longjmp_table
        .quad __guard_longjmp_count
        .fill 84, 1, 0

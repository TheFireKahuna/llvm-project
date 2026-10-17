; REQUIRES: x86
;; Under -import-slots, a DLL's vague entities that another image's instance
;; may preempt stay in import form through LTO, so that the DLL's references
;; read the cells its fill writes. -Bsymbolic makes them final, and LTO
;; reaches them directly.

; RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
; RUN: llvm-as a.ll -o a.obj
; RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium crt.s -o crt.obj
; RUN: lld-link -def:ntdll.def -out:ntdll.lib -machine:x64
; RUN: lld-link -import-slots -dll -noentry a.obj crt.obj ntdll.lib -out:a.dll
; RUN: llvm-objdump -d a.dll | FileCheck --check-prefix=CELLS %s
; RUN: lld-link -import-slots -dll -noentry a.obj crt.obj ntdll.lib \
; RUN:   -Bsymbolic -out:local.dll
; RUN: llvm-objdump -d local.dll | FileCheck --check-prefix=LOCAL %s

; CELLS:      <get>:
; CELLS-NEXT: movq {{.*}}(%rip), %rax
; CELLS-NEXT: movl (%rax), %eax

; LOCAL:      <get>:
; LOCAL-NEXT: movl {{.*}}(%rip), %eax # {{.*}} <v>

;--- a.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-windows-itanium"

$v = comdat any

@v = linkonce_odr global i32 1, comdat

define dllexport i32 @get() {
  %x = load i32, ptr @v
  ret i32 %x
}

;--- crt.s
  .section .CRT$XIA,"dr"
  .globl __xi_a
__xi_a:
  .quad 0
  .section .CRT$XIZ,"dr"
  .globl __xi_z
__xi_z:
  .quad 0

;--- ntdll.def
LIBRARY ntdll.dll
EXPORTS
  LdrAddRefDll
  LdrGetProcedureAddressForCaller
  NtProtectVirtualMemory

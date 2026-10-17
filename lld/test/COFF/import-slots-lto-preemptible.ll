; REQUIRES: x86
;; Under -import-slots, a DLL's vague entities that another image's instance
;; may preempt stay in import form through LTO, so that the DLL's references
;; read the cells its fill writes: a variable's address through its pointer, a
;; thread-local variable's record through the record's. -Bsymbolic makes them
;; final, and LTO reaches them directly, the thread-local one through the
;; image's TLS index.

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
; CELLS-NEXT: movq {{.*}}(%rip), %rcx
; CELLS-NEXT: movq (%rcx), %rdx
; CELLS-NEXT: movl (%rdx), %edx
; CELLS-NEXT: movq %gs:0x58, %r8
; CELLS-NEXT: movq (%r8,%rdx,8), %rdx
; CELLS-NEXT: movl 0x8(%rcx), %ecx

; LOCAL:      <get>:
; LOCAL-NEXT: movl {{.*}}(%rip), %eax # {{.*}} <v>
; LOCAL-NEXT: movl {{.*}}(%rip), %ecx
; LOCAL-NEXT: movq %gs:0x58, %rdx
; LOCAL-NEXT: movq (%rdx,%rcx,8), %rcx

;--- a.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-windows-itanium"

$v = comdat any
$t = comdat any

@v = linkonce_odr global i32 1, comdat
@t = linkonce_odr thread_local global i32 2, comdat

define dllexport i32 @get() {
  %x = load i32, ptr @v
  %p = call ptr @llvm.threadlocal.address.p0(ptr @t)
  %y = load i32, ptr %p
  %s = add i32 %x, %y
  ret i32 %s
}

declare ptr @llvm.threadlocal.address.p0(ptr)

;--- crt.s
  .section .CRT$XIA,"dr"
  .globl __xi_a
__xi_a:
  .quad 0
  .section .CRT$XIZ,"dr"
  .globl __xi_z
__xi_z:
  .quad 0
  .data
  .globl _tls_index
_tls_index:
  .long 0

;--- ntdll.def
LIBRARY ntdll.dll
EXPORTS
  LdrAddRefDll
  LdrGetProcedureAddressForCaller
  NtProtectVirtualMemory

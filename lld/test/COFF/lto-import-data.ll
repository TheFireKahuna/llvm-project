; REQUIRES: aarch64, x86
;; Under -import-slots, a bitcode reference to a variable that is not
;; dso_local becomes an import-form reference, so the import of the variable,
;; which offers only __imp_var, satisfies it before LTO as after it.

; RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
; RUN: llvm-as x86.ll -o x86.bc
; RUN: llvm-as arm64.ll -o arm64.bc
; RUN: lld-link -def:imp.def -out:imp-x86.lib -machine:x64
; RUN: lld-link -def:imp.def -out:imp-arm64.lib -machine:arm64

; RUN: lld-link -import-slots -dll -noentry -export:use -out:x86.dll x86.bc \
; RUN:   imp-x86.lib
; RUN: llvm-objdump -d --no-show-raw-insn x86.dll | \
; RUN:   FileCheck --check-prefix=X86 %s
; RUN: lld-link -machine:arm64 -import-slots -dll -noentry -export:use \
; RUN:   -out:arm64.dll arm64.bc imp-arm64.lib
; RUN: llvm-objdump -d --no-show-raw-insn arm64.dll | \
; RUN:   FileCheck --check-prefix=ARM64 %s

;; Without -import-slots, nothing satisfies the reference to var.
; RUN: not lld-link -dll -noentry -export:use -out:x86.dll x86.bc imp-x86.lib \
; RUN:   2>&1 | FileCheck --check-prefix=ERR %s

; X86-LABEL: <use>:
; X86-NEXT:  movq 0x{{[0-9a-f]+}}(%rip), %rax
; X86-NEXT:  movl (%rax), %eax

; ARM64-LABEL: <use>:
; ARM64-NEXT:  adrp x8, 0x{{[0-9a-f]+}}
; ARM64-NEXT:  ldr x8, [x8, #0x{{[0-9a-f]+}}]
; ARM64-NEXT:  ldr w0, [x8]

; ERR: error: undefined symbol: var

;--- x86.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-windows-itanium"

@var = external global i32

define i32 @use() {
  %v = load i32, ptr @var
  ret i32 %v
}

;--- arm64.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-p:64:64-i32:32-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "aarch64-unknown-windows-itanium"

@var = external global i32

define i32 @use() {
  %v = load i32, ptr @var
  ret i32 %v
}

;--- imp.def
LIBRARY imp.dll
EXPORTS
  var DATA

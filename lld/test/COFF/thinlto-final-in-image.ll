; REQUIRES: aarch64, x86
;; Under ThinLTO, a dllimport reference to a definition in a regular object is
;; final in the image, so the backend emits it as a direct reference, as
;; regular LTO does.

; RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
; RUN: opt -module-summary x86.ll -o x86.bc
; RUN: llc x86-other.ll -filetype=obj -o x86-other.obj
; RUN: lld-link -dll -noentry -export:use -out:x86.dll x86.bc x86-other.obj
; RUN: llvm-objdump -d --no-show-raw-insn x86.dll | \
; RUN:   FileCheck --check-prefix=X86 %s

; RUN: opt -module-summary arm64.ll -o arm64.bc
; RUN: llc arm64-other.ll -filetype=obj -o arm64-other.obj
; RUN: lld-link -machine:arm64 -dll -noentry -export:use -out:arm64.dll \
; RUN:   arm64.bc arm64-other.obj
; RUN: llvm-objdump -d --no-show-raw-insn arm64.dll | \
; RUN:   FileCheck --check-prefix=ARM64 %s

; X86-LABEL: <use>:
; X86-NOT:   addr32
; X86:       callq 0x{{[0-9a-f]+}} <.text>
; X86-NEXT:  addl 0x{{[0-9a-f]+}}(%rip), %eax

; ARM64-LABEL: <use>:
; ARM64-NOT:   blr
; ARM64:       bl 0x{{[0-9a-f]+}} <.text>
; ARM64-NEXT:  adrp x8, 0x{{[0-9a-f]+}}
; ARM64-NEXT:  ldr w8, [x8]

;--- x86.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

declare dllimport i32 @innative()
@vnative = external dllimport global i32

define i32 @use() {
  %a = call i32 @innative()
  %b = load i32, ptr @vnative
  %s = add i32 %a, %b
  ret i32 %s
}

;--- x86-other.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

define i32 @innative() {
  ret i32 3
}

@vnative = global i32 5

;--- arm64.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-p:64:64-i32:32-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "aarch64-pc-windows-msvc"

declare dllimport i32 @innative()
@vnative = external dllimport global i32

define i32 @use() {
  %a = call i32 @innative()
  %b = load i32, ptr @vnative
  %s = add i32 %a, %b
  ret i32 %s
}

;--- arm64-other.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-p:64:64-i32:32-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "aarch64-pc-windows-msvc"

define i32 @innative() {
  ret i32 3
}

@vnative = global i32 5

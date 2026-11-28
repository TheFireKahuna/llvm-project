; REQUIRES: x86

;; -Bsymbolic binds a DLL's vague entities to its own definitions instead of an
;; imported DLL's (-functions only functions, non-weak kinds none); a unique
;; variable stays one; the last option decides, and an executable binds nothing.

; RUN: rm -rf %t && split-file %s %t && cd %t
; RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium %S/Inputs/fill-entry.s -o fill.obj
; RUN: cat a.ll get.ll > dll.ll && cat a.ll main.ll > exe.ll
; RUN: llc -filetype=obj dll.ll -o dll.obj && llc -filetype=obj exe.ll -o exe.obj
; RUN: lld-link -dll -noentry fill.obj dll.obj -out:a.dll -implib:a.lib

; DEFINE: %{kind} =
; DEFINE: %{keeps} = ALL
; DEFINE: %{check} = lld-link -dll -noentry fill.obj dll.obj a.lib -out:b.dll %{kind} && \
; DEFINE:   llvm-readobj --coff-exports b.dll | FileCheck %s \
; DEFINE:     --check-prefixes=CHECK,%{keeps}

; RUN: %{check}
; REDEFINE: %{kind} = -Bsymbolic-non-weak
; RUN: %{check}
; REDEFINE: %{kind} = -Bsymbolic-non-weak-functions
; RUN: %{check}
; REDEFINE: %{kind} = -Bsymbolic -Bno-symbolic
; RUN: %{check}
; REDEFINE: %{kind} = -Bsymbolic-functions
; REDEFINE: %{keeps} = DATA
; RUN: %{check}
; REDEFINE: %{kind} = -Bno-symbolic -Bsymbolic
; REDEFINE: %{keeps} = UNIQUE
; RUN: %{check}

;; What the DLL keeps it exports; the rest it forwards to the instance's DLL.
; CHECK:        Name: fn
; ALL-NEXT:     ForwardedTo: a.dll.fn
; DATA-NEXT:    RVA:
; UNIQUE-NEXT:  RVA:
; CHECK:        Name: iv
; ALL-NEXT:     ForwardedTo: a.dll.iv
; DATA-NEXT:    ForwardedTo: a.dll.iv
; UNIQUE-NEXT:  RVA:
; CHECK:        Name: u
; CHECK-NEXT:   ForwardedTo: a.dll.u

; RUN: lld-link -entry:main -subsystem:console exe.obj a.lib -out:exe.exe \
; RUN:   -Bsymbolic
; RUN: llvm-readobj --coff-imports exe.exe | FileCheck --check-prefix=EXE %s
; EXE-DAG: Symbol: fn (
; EXE-DAG: Symbol: iv (
; EXE-DAG: Symbol: u (

;--- a.ll
target triple = "x86_64-unknown-windows-itanium"

$fn = comdat any
$iv = comdat any
$u = comdat any

@iv = linkonce_odr global i32 0, comdat
@u = linkonce_odr global i32 0, comdat #0
@p = global ptr @fn

define linkonce_odr void @fn() comdat {
  ret void
}

define ptr @use() {
  %a = load volatile i32, ptr @iv
  %b = load volatile i32, ptr @u
  %c = load volatile ptr, ptr @p
  ret ptr %c
}

attributes #0 = { "gnu-unique" }

;--- get.ll
define dllexport ptr @get() {
  %r = call ptr @use()
  ret ptr %r
}

;--- main.ll
define ptr @main() {
  %r = call ptr @use()
  ret ptr %r
}

; REQUIRES: x86, aarch64

;; A vague type_info is offered and bound like a vague variable: the image holds
;; the DLL's (its words are slots, its copy and name leave); the name is no
;; entity, and a kept type_info points at its own image's name.

; RUN: rm -rf %t && split-file %s %t && cd %t
; RUN: cat a.ll b.ll > main.ll

; DEFINE: %{t} = x86_64
; DEFINE: %{llc} = llc -mtriple=%{t}-windows-itanium -filetype=obj
; DEFINE: %{run} = %{llc} a.ll -o a.obj && %{llc} dll.ll -o dll.obj && \
; DEFINE:   %{llc} main.ll -o main.obj && \
; DEFINE:   lld-link -dll -noentry a.obj dll.obj -out:a.dll -implib:a.lib && \
; DEFINE:   llvm-readobj --coff-exports a.dll | \
; DEFINE:     FileCheck --check-prefix=EXPORTS %s --implicit-check-not=_ZTS && \
; DEFINE:   lld-link -entry:main -subsystem:console -out:exe.exe \
; DEFINE:     -lldmap:exe.map main.obj a.lib && \
; DEFINE:   llvm-readobj --coff-imports --coff-basereloc exe.exe | \
; DEFINE:     FileCheck --check-prefix=IMPORTS %s && \
; DEFINE:   FileCheck --check-prefix=MAP --input-file=exe.map %s \
; DEFINE:     --implicit-check-not="for A"

; RUN: %{run}
; REDEFINE: %{t} = aarch64
; RUN: %{run}

; EXPORTS: Name: _ZTI1A
; EXPORTS: Name: get

;; The two slots, and the one relocated address: the kept object's name.
; IMPORTS:      Symbol: _ZTI1A (
; IMPORTS:      Symbol: _ZTI1A (
; IMPORTS-NOT:  Symbol:
; IMPORTS:      BaseReloc [
; IMPORTS-NEXT:   Entry {
; IMPORTS-NEXT:     Type: DIR64
; IMPORTS-NEXT:     Address:
; IMPORTS-NEXT:   }
; IMPORTS-NOT:      Type: DIR64

; MAP: typeinfo for B
; MAP: vtable for B
; MAP: typeinfo name for B

;--- a.ll
$_ZTI1A = comdat any
$_ZTS1A = comdat any

@_ZTS1A = linkonce_odr constant [3 x i8] c"1A\00", comdat
@_ZTI1A = linkonce_odr constant { ptr, ptr } { ptr null, ptr @_ZTS1A }, comdat #0

attributes #0 = { "type-info" }

;--- dll.ll
@_ZTI1A = external constant { ptr, ptr }

define dllexport ptr @get() {
  ret ptr @_ZTI1A
}

;--- b.ll
$_ZTI1B = comdat any
$_ZTS1B = comdat any
$_ZTV1B = comdat any

@_ZTS1B = linkonce_odr constant [3 x i8] c"1B\00", comdat
@_ZTI1B = linkonce_odr constant { ptr, ptr, ptr } { ptr null, ptr @_ZTS1B, ptr @_ZTI1A }, comdat #0
@_ZTV1B = linkonce_odr dso_local unnamed_addr constant [2 x ptr] [ptr @_ZTI1A, ptr null], comdat

define ptr @main() {
  %a = load volatile ptr, ptr @_ZTV1B
  %b = load volatile ptr, ptr @_ZTI1B
  ret ptr @_ZTI1A
}

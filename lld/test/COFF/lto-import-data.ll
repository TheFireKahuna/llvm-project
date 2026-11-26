; REQUIRES: x86, aarch64

;; Fork bitcode names X where its LTO code may use __imp_X, so an import offering
;; only __imp_X loads for undefined X; a reference LTO leaves direct is reported
;; then, and foreign bitcode keeps the error before LTO. A definition LTO drops
;; asks for no import after it.

; RUN: rm -rf %t && split-file %s %t && cd %t

; DEFINE: %{a} = x64
; DEFINE: %{t} = x86_64
; DEFINE: %{link} = lld-link -machine:%{a} -dll -noentry imp.lib
; DEFINE: %{run} = \
; DEFINE:   opt -mtriple=%{t}-unknown-windows-itanium main.ll -o fork.bc && \
; DEFINE:   opt -mtriple=%{t}-pc-windows-msvc main.ll -o foreign.bc && \
; DEFINE:   lld-link -machine:%{a} -def:imp.def -out:imp.lib && \
; DEFINE:   %{link} -export:zero -out:zero.dll fork.bc -verbose 2>&1 | \
; DEFINE:     FileCheck --check-prefix=ACCEPT %s && \
; DEFINE:   not %{link} -export:use -out:use.dll fork.bc 2>&1 | \
; DEFINE:     FileCheck --check-prefix=DIRECT %s && \
; DEFINE:   not %{link} -export:zero -out:zero.dll foreign.bc 2>&1 | \
; DEFINE:     FileCheck --check-prefix=FOREIGN %s

; RUN: %{run}
; REDEFINE: %{a} = arm64
; REDEFINE: %{t} = aarch64
; RUN: %{run}

; ACCEPT:     Loading lazy __imp_var from imp.lib for automatic import
; ACCEPT-NOT: error:
; ACCEPT-NOT: __imp_gone

; DIRECT: error: {{.*}}.lto.obj: var is provided by imp.dll, but

; FOREIGN: error: undefined symbol: var

;--- main.ll
@var = external dso_local global i32

define i32 @use() {
  %v = load i32, ptr @var
  ret i32 %v
}

define i32 @zero() {
  ret i32 0
}

@gone = linkonce_odr hidden alias void (), ptr @impl

define internal void @impl() {
  ret void
}

;--- imp.def
LIBRARY imp.dll
EXPORTS
  var DATA
  gone

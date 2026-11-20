; REQUIRES: x86, aarch64
;; llvm.used keeps a local (sectioned without COMDAT) or external global through
;; /opt:ref, llvm.compiler.used does not; a COMDAT variable's initializer runs
;; for an unreferenced variable only where llvm.used names it.

; RUN: rm -rf %t && split-file %s %t && cd %t
; DEFINE: %{t} = x86_64
; DEFINE: %{run} = llc -filetype=obj -mtriple=%{t}-unknown-windows-itanium \
; DEFINE:     -data-sections -function-sections a.ll -o a.obj && \
; DEFINE:   lld-link -entry:main -subsystem:console -opt:ref -out:a.exe \
; DEFINE:     a.obj -map:a.map && \
; DEFINE:   FileCheck %s --implicit-check-not='{{ }}cu{{ }}' \
; DEFINE:     --implicit-check-not='{{ }}xcu{{ }}' \
; DEFINE:     --implicit-check-not='{{ }}init2{{ }}' < a.map
; RUN: %{run}
; REDEFINE: %{t} = aarch64
; RUN: %{run}

; CHECK:      {{ }}xf{{ }}
; CHECK:      {{ }}xloc{{ }}
; CHECK:      Static symbols
; CHECK-EMPTY:
; CHECK-NEXT: {{ }}lf{{ }}
; CHECK-NEXT: {{ }}init{{ }}
; CHECK-NEXT: {{ }}loc{{ }}

;--- a.ll
@loc = internal global i32 2
@cu = internal global i32 3
@xloc = global i32 4
@xcu = global i32 5

define internal void @lf() {
  ret void
}

$key = comdat any
$key2 = comdat any
@key = linkonce_odr global i32 0, comdat
@key2 = linkonce_odr global i32 0, comdat
@llvm.global_ctors = appending global [2 x { i32, ptr, ptr }] [{ i32, ptr, ptr } { i32 65535, ptr @init, ptr @key }, { i32, ptr, ptr } { i32 65535, ptr @init2, ptr @key2 }]

define internal void @init() {
  store i32 1, ptr @key
  ret void
}

define internal void @init2() {
  store i32 1, ptr @key2
  ret void
}

define void @xf() {
  ret void
}

define i32 @main() {
  ret i32 0
}

@llvm.used = appending global [5 x ptr] [ptr @loc, ptr @lf, ptr @xloc, ptr @xf, ptr @key], section "llvm.metadata"
@llvm.compiler.used = appending global [2 x ptr] [ptr @cu, ptr @xcu], section "llvm.metadata"

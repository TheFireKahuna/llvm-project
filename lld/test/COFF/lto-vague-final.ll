; REQUIRES: x86

;; Offers resolve before LTO: an offered dynamically initialized variable is not
;; prevailing (no copy compiled), constants prevail but are not final, functions
;; prevail and are final; full and thin LTO import what a plain build does. A
;; DLL's own preemptible variables, constants and guards are not final either,
;; since it reaches them through cells, unless -Bsymbolic binds them; its
;; function is, since only its address-takes read the cell.

; RUN: rm -rf %t && split-file %s %t && cd %t
; RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium %S/Inputs/fill-entry.s -o fill.obj
; RUN: cat a.ll main.ll > exe.ll && cat a.ll get.ll > dll.ll
; RUN: llc -filetype=obj dll.ll -o dll.obj && llc -filetype=obj exe.ll -o exe.obj
; RUN: opt exe.ll -o exe.bc && opt -module-hash -module-summary exe.ll -o thin.bc
; RUN: lld-link -dll -noentry fill.obj dll.obj -out:a.dll -implib:a.lib

; DEFINE: %{in} = exe.bc a.lib
; DEFINE: %{obj} = exe.exe.lto.obj
; DEFINE: %{link} = lld-link -entry:main -subsystem:console -out:exe.exe \
; DEFINE:   -lldmap:exe.map
; DEFINE: %{check} = %{link} -lldsavetemps %{in} && \
; DEFINE:   FileCheck --input-file=exe.exe.resolution.txt %s && \
; DEFINE:   llvm-nm %{obj} | FileCheck --check-prefix=OBJECT %s && \
; DEFINE:   llvm-readobj --coff-imports exe.exe | \
; DEFINE:     FileCheck --check-prefix=IMPORTS %s && \
; DEFINE:   FileCheck --check-prefix=GONE --input-file=exe.map %s \
; DEFINE:     --implicit-check-not='.rdata$c' --implicit-check-not='.data$m'

; RUN: %{check}
; REDEFINE: %{in} = a.lib exe.bc
; RUN: %{check}
; REDEFINE: %{in} = thin.bc a.lib
; REDEFINE: %{obj} = exe.exe.lto.thin.obj
; RUN: %{check}

; CHECK: ,fn,plx{{$}}
; CHECK: ,c,px{{$}}
; CHECK: ,m,px{{$}}
; CHECK: ,g,x{{$}}
; CHECK: ,_ZGV1g,x{{$}}

; OBJECT-DAG: U __imp__ZGV1g
; OBJECT-DAG: U __imp_c
; OBJECT-DAG: U __imp_g
; OBJECT-DAG: U __imp_m
; OBJECT-DAG: R c
; OBJECT-DAG: T fn
; OBJECT-DAG: D m

; IMPORTS-DAG: Symbol: _ZGV1g (
; IMPORTS-DAG: Symbol: c (
; IMPORTS-DAG: Symbol: fn (
; IMPORTS-DAG: Symbol: g (
; IMPORTS-DAG: Symbol: m (

; GONE: lto

; RUN: %{link} exe.obj a.lib
; RUN: llvm-readobj --coff-imports exe.exe | FileCheck --check-prefix=IMPORTS %s

; RUN: opt dll.ll -o dll.bc
; RUN: lld-link -dll -noentry fill.obj dll.bc -out:lto.dll -lldsavetemps
; RUN: FileCheck --check-prefix=DLL --input-file=lto.dll.resolution.txt %s
; RUN: lld-link -dll -noentry fill.obj dll.bc -out:sym.dll -lldsavetemps \
; RUN:   -Bsymbolic
; RUN: FileCheck --check-prefix=SYM --input-file=sym.dll.resolution.txt %s
; DLL: ,fn,plx{{$}}
; DLL: ,c,px{{$}}
; DLL: ,m,px{{$}}
; DLL: ,g,px{{$}}
; DLL: ,_ZGV1g,px{{$}}
; SYM: ,c,plx{{$}}
; SYM: ,m,plx{{$}}
; SYM: ,g,plx{{$}}
; SYM: ,_ZGV1g,plx{{$}}

;; The thin LTO cache keys on the resolutions: taking no offer compiles another
;; entry, taking them again does not.
; RUN: mkdir cache && %{link} thin.bc a.lib -lldltocache:cache
; RUN: ls cache/llvmcache-* | count 1
; RUN: %{link} thin.bc a.lib -lldltocache:cache -ignore-offers:a.dll
; RUN: ls cache/llvmcache-* | count 2
; RUN: %{link} thin.bc a.lib -lldltocache:cache
; RUN: ls cache/llvmcache-* | count 2

;--- a.ll
target triple = "x86_64-unknown-windows-itanium"

$c = comdat any
$m = comdat any
$g = comdat any
$fn = comdat any

@c = linkonce_odr constant [2 x i32] [i32 1, i32 2], comdat
@m = linkonce_odr global i32 5, comdat
@g = linkonce_odr global i32 0, comdat #0
@_ZGV1g = linkonce_odr global i64 0, comdat($g)
@p = global ptr @fn

define linkonce_odr i32 @fn() comdat {
  ret i32 7
}

define i32 @use() {
  %a = load volatile i32, ptr @c
  %b = load volatile i32, ptr @m
  %d = load volatile i32, ptr @g
  %e = load volatile i64, ptr @_ZGV1g
  %q = load volatile ptr, ptr @p
  %f = call i32 @fn()
  ret i32 %f
}

attributes #0 = { "dynamic-init"="guarded" }

;--- main.ll
define i32 @main() {
  %r = call i32 @use()
  ret i32 %r
}

;--- get.ll
define dllexport i32 @get() {
  %r = call i32 @use()
  ret i32 %r
}

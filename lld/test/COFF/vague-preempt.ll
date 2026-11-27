; REQUIRES: x86, aarch64

;; A definition of a vague variable that a DLL offers gives way: its whole
;; COMDAT group (guard, interior names, initializer) becomes the DLL's imports
;; in every input order and form; an export becomes a forwarder.

; RUN: rm -rf %t && split-file %s %t && cd %t
; RUN: cat a.ll alias.ll > more.ll

; DEFINE: %{t} = x86_64
; DEFINE: %{llc} = llc -mtriple=%{t}-windows-itanium -filetype=obj
; DEFINE: %{opt} = opt -mtriple=%{t}-windows-itanium
; DEFINE: %{link} = lld-link -entry:main -subsystem:console -out:exe.exe \
; DEFINE:   -lldmap:exe.map
; DEFINE: %{gone} = llvm-readobj --coff-imports exe.exe | \
; DEFINE:     FileCheck --check-prefix=IMPORTS %s && \
; DEFINE:   FileCheck --check-prefix=GONE --input-file=exe.map %s \
; DEFINE:     --implicit-check-not=iv --implicit-check-not=init \
; DEFINE:     --implicit-check-not=CRT
; DEFINE: %{kept} = llvm-readobj --coff-imports exe.exe | \
; DEFINE:     FileCheck --check-prefix=NONE --allow-empty %s && \
; DEFINE:   FileCheck --check-prefix=KEPT --input-file=exe.map %s
; DEFINE: %{run} = %{llc} a.ll -o a.obj && %{llc} dll.ll -o dll.obj && \
; DEFINE:   %{llc} main.ll -o main.obj && %{llc} more.ll -o more.obj && \
; DEFINE:   %{llc} direct.ll -o direct.obj && %{llc} guard.ll -o guard.obj && \
; DEFINE:   %{opt} a.ll -o a.bc && %{opt} main.ll -o main.bc && \
; DEFINE:   %{opt} -module-summary a.ll -o a.thin.bc && \
; DEFINE:   %{opt} -module-summary main.ll -o main.thin.bc && \
; DEFINE:   rm -f a.a && llvm-ar rcs a.a a.obj && \
; DEFINE:   lld-link -dll -noentry a.obj dll.obj -out:a.dll -implib:a.lib && \
; DEFINE:   lld-link -dll -noentry guard.obj -out:g.dll -implib:g.lib && \
; DEFINE:   %{link} a.obj main.obj a.lib && %{gone} && \
; DEFINE:   %{link} -opt:noref -debug -include:iv a.lib main.obj a.obj && \
; DEFINE:   %{gone} && %{link} main.obj a.lib a.a && %{gone} && \
; DEFINE:   %{link} -opt:noref a.bc main.obj a.lib && %{gone} && \
; DEFINE:   %{link} -opt:noref a.lib main.thin.bc a.thin.bc && %{gone} && \
; DEFINE:   %{link} -opt:noref a.bc a.obj main.bc a.lib && %{gone} && \
; DEFINE:   %{link} a.obj main.obj a.lib -export:iv,DATA && \
; DEFINE:   llvm-readobj --coff-exports exe.exe | \
; DEFINE:     FileCheck --check-prefix=FORWARD %s && \
; DEFINE:   %{link} a.obj main.obj a.lib -delayload:a.dll && %{kept} && \
; DEFINE:   %{link} a.obj main.obj a.lib -version-script:iv.map && %{kept} && \
; DEFINE:   %{link} a.a main.obj a.lib -exclude-libs:a.a && %{kept} && \
; DEFINE:   not %{link} more.obj main.obj a.lib 2>&1 | \
; DEFINE:     FileCheck --check-prefix=PART %s --implicit-check-not=error: && \
; DEFINE:   not %{link} a.obj main.obj g.lib 2>&1 | \
; DEFINE:     FileCheck --check-prefix=MEMBER %s --implicit-check-not=error: && \
; DEFINE:   not %{link} -opt:noref a.obj main.obj direct.obj a.lib 2>&1 | \
; DEFINE:     FileCheck --check-prefix=DIRECT %s

; RUN: %{run}
; REDEFINE: %{t} = aarch64
; RUN: %{run}

;; The guard's pointer is read by code; the words holding iv's address and an
;; address inside it are in-place slots.
; IMPORTS: Name: a.dll
; IMPORTS: Symbol: _ZGV2iv (
; IMPORTS: Symbol: iv (
; IMPORTS: Symbol: iv$so4 (

; GONE: main

; FORWARD:      Name: iv
; FORWARD-NEXT: ForwardedTo: a.dll.iv

;; A delay-loaded DLL's offer is not taken, nor does a version-script-local or
;; --exclude-libs definition give way: the image keeps its copy.
; NONE-NOT: Symbol:
; KEPT:     a.obj:(.CRT$XCU)

;; A group moves whole: a group name the DLL does not offer, or an offered
;; guard without its variable, is an error.
; PART: error: more.obj: iv is provided by a.dll, but iv2, which its COMDAT group also defines, is not; give both the visibility they have in a.dll
; MEMBER: error: a.obj: guard variable for iv is provided by g.dll, which does not provide the vague entity whose COMDAT group defines it; give both the visibility they have in that DLL

;; Code compiled to reach the variable locally cannot follow it to the DLL.
; DIRECT: error: direct.obj: iv is provided by a.dll, but is referenced as if it were local; recompile direct.obj, or give iv hidden visibility

;; An offer an ARM64EC image would take is an error.
; RUN: llc -mtriple=x86_64-windows-itanium -filetype=obj a.ll -o a.obj
; RUN: llc -mtriple=x86_64-windows-itanium -filetype=obj main.ll -o main.obj
; RUN: llc -mtriple=x86_64-windows-itanium -filetype=obj dll.ll -o dll.obj
; RUN: lld-link -dll -noentry a.obj dll.obj -out:a.dll -implib:a.lib
; RUN: not lld-link -machine:arm64ec -dll -noentry a.obj main.obj a.lib \
; RUN:   -out:ec.dll 2>&1 | FileCheck --check-prefix=EC %s
; EC: error: a.obj: iv is a vague entity, which an ARM64EC or ARM64X image cannot share with another; give it hidden visibility

;--- iv.map
{ local: *iv*; };

;--- a.ll
$iv = comdat any

@iv = linkonce_odr global [2 x i32] zeroinitializer, comdat
@_ZGV2iv = linkonce_odr global i64 0, comdat($iv)
@"iv$so4" = alias i32, getelementptr (i32, ptr @iv, i64 1)
@llvm.global_ctors = appending global [1 x { i32, ptr, ptr }] [{ i32, ptr, ptr } { i32 65535, ptr @init, ptr @iv }]

define internal void @init() comdat($iv) {
  store i32 1, ptr @iv
  store i64 1, ptr @_ZGV2iv
  ret void
}

;--- alias.ll
@iv2 = alias i32, ptr @iv

;--- dll.ll
@iv = external global [2 x i32]

define dllexport ptr @get() {
  ret ptr @iv
}

;--- guard.ll
@_ZGV2iv = dllexport global i64 0

;--- main.ll
@iv = external global [2 x i32]
@_ZGV2iv = external global i64
@p = global ptr @iv
@q = global ptr getelementptr (i8, ptr @iv, i64 4)

define i64 @main() {
  %a = load i64, ptr @_ZGV2iv
  %b = load volatile ptr, ptr @p
  %c = load volatile ptr, ptr @q
  ret i64 %a
}

define void @__delayLoadHelper2() {
  ret void
}

;--- direct.ll
@iv = external dso_local global [2 x i32]

define i32 @direct() {
  %a = load i32, ptr @iv
  ret i32 %a
}

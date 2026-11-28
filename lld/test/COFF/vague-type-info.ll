; REQUIRES: x86, aarch64

;; A vague type_info is offered and bound like a vague variable: the image holds
;; the DLL's (its words are slots, its copy and name leave); the name is no
;; entity, and a kept type_info points at its own image's name.
;;
;; One whose name word holds the non-unique bit, which clang sets for a vague
;; type_info that is not hidden, is an entity of the Images record, its size
;; the length of its name's blob, which the name word states, and its layout
;; shape the blob after the name and its NUL. A DLL that keeps it preemptible has its name word, the word at
;; 8 of the object, which moves to .sealed, as a cell of its fill, of use 4,
;; and its offer table, which says that it holds one, names the object. An
;; executable that sees two DLLs keep it imports the owner's, which its table
;; names; two layouts of one name are an error between them, and along an
;; import path, by their bytes: the two here share the low 32 bits of their
;; blobs' xxh3 hash.

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

; DEFINE: %{names} = %{llc} f.ll -o f.obj && %{llc} g.ll -o g.obj && \
; DEFINE:   %{llc} f2.ll -o f2.obj && %{llc} main2.ll -o main2.obj && \
; DEFINE:   %{llc} main3.ll -o main3.obj && \
; DEFINE:   llvm-mc -filetype=obj -triple=%{t}-windows-itanium \
; DEFINE:     %S/Inputs/fill-entry.s -o fill.obj && \
; DEFINE:   lld-link -dll -noentry fill.obj f.obj -out:d1.dll -implib:d1.lib && \
; DEFINE:   lld-link -dll -noentry fill.obj f2.obj -out:d2.dll -implib:d2.lib && \
; DEFINE:   lld-link -dll -noentry fill.obj g.obj -out:d3.dll -implib:d3.lib && \
; DEFINE:   llvm-readobj --coff-link-records d1.lib | \
; DEFINE:     FileCheck --check-prefix=RECORD %s && \
; DEFINE:   llvm-objdump -s -j .rdata -j .sealed d1.dll | \
; DEFINE:     FileCheck --check-prefix=NAMES %s && \
; DEFINE:   lld-link -entry:main -subsystem:console main2.obj d1.lib d2.lib \
; DEFINE:     -out:two.exe && \
; DEFINE:   llvm-readobj --coff-imports two.exe | \
; DEFINE:     FileCheck --check-prefix=OWNER %s && \
; DEFINE:   not lld-link -entry:main -subsystem:console main2.obj d1.lib \
; DEFINE:     d3.lib -out:bad.exe 2>&1 | FileCheck --check-prefix=SIBLING %s && \
; DEFINE:   not lld-link -entry:main -subsystem:console main3.obj d1.lib \
; DEFINE:     -out:bad.exe 2>&1 | FileCheck --check-prefix=PATH %s

; REDEFINE: %{t} = x86_64
; RUN: %{names}
; REDEFINE: %{t} = aarch64
; RUN: %{names}

; RECORD:      Name: _ZTI1F
; RECORD-NEXT: Visibility: Default
; RECORD-NEXT: Kind: Descriptor
; RECORD-NEXT: Binding: Preemptible
; RECORD-NEXT: Size: 11
; RECORD-NEXT: Alignment: 1
; RECORD-NEXT: Shape: (8E F0 59 43 7D 51 36 1C)

;; The list: one cell, one word, the sealed section, the entry that names
;; d1.dll and one name word, at 0x3010 in .sealed, of use 4 below the top bits
;; of its key, with its entry and the name 1F, which it holds. The table holds
;; a type_info, which its entry names at 0x3008, of size 11, preemptible.
; NAMES:      Contents of section .rdata:
; NAMES-NEXT: 180002000 3146008e f059437d 51361c00 00000000
; NAMES-NEXT: 180002010 00000000 01000000 01000000 01000000
; NAMES-NEXT: 180002020 00300000 18000000 a0200000 00000000
; NAMES-NEXT: 180002030 10300000 24372bf3 b8200000 00200000
; NAMES:      180002090 2b1d7a4f 01000000 02000000 01000000
; NAMES:      1800020b0 {{.*}} c033607d 3a372bf3
; NAMES-NEXT: 1800020c0 {{[0-9a-f]+}} 08300000 0b000000 01220000
; NAMES:      Contents of section .sealed:
; NAMES-NEXT: 180003000 00000000 00000000 00000000 00000000
; NAMES-NEXT: 180003010 00200080 01000b80

; OWNER:      Name: d1.dll
; OWNER:      Symbol: F (0)
; OWNER-NEXT: Symbol: _ZTI1F (1)

; SIBLING: error: typeinfo for F is a type whose layout hashes to 1334E302A9C7A083 in d3.dll and a type whose layout hashes to F1019380A414A9E9 in d1.dll, whose copy the program shares

; PATH: error: main3.obj: typeinfo for F is a type whose layout hashes to 1334E302A9C7A083, and a type whose layout hashes to F1019380A414A9E9 in d1.dll, which provides it

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

;--- f.ll
$_ZTI1F = comdat any
$_ZTS1F = comdat exactmatch

@_ZTS1F = linkonce_odr constant [11 x i8] c"1F\00\8E\F0\59\43\7D\51\36\1C", comdat
@_ZTI1F = linkonce_odr constant { ptr, ptr } { ptr null, ptr inttoptr (i64 add (i64 ptrtoint (ptr @_ZTS1F to i64), i64 -9220275812110958592) to ptr) }, comdat #0

define dllexport ptr @F() {
  ret ptr @_ZTI1F
}

attributes #0 = { "type-info" }

;--- f2.ll
$_ZTI1F = comdat any
$_ZTS1F = comdat exactmatch

@_ZTS1F = linkonce_odr constant [11 x i8] c"1F\00\8E\F0\59\43\7D\51\36\1C", comdat
@_ZTI1F = linkonce_odr constant { ptr, ptr } { ptr null, ptr inttoptr (i64 add (i64 ptrtoint (ptr @_ZTS1F to i64), i64 -9220275812110958592) to ptr) }, comdat #0

define dllexport ptr @G() {
  ret ptr @_ZTI1F
}

attributes #0 = { "type-info" }

;--- g.ll
$_ZTI1F = comdat any
$_ZTS1F = comdat exactmatch

@_ZTS1F = linkonce_odr constant [11 x i8] c"1F\00\07\5A\6D\40\7C\72\54\9F", comdat
@_ZTI1F = linkonce_odr constant { ptr, ptr } { ptr null, ptr inttoptr (i64 add (i64 ptrtoint (ptr @_ZTS1F to i64), i64 -9220275812110958592) to ptr) }, comdat #0

define dllexport ptr @G() {
  ret ptr @_ZTI1F
}

attributes #0 = { "type-info" }

;--- main2.ll
declare dllimport ptr @F()
declare dllimport ptr @G()

define i32 @main() {
  call ptr @F()
  call ptr @G()
  ret i32 0
}

;--- main3.ll
$_ZTI1F = comdat any
$_ZTS1F = comdat exactmatch

@_ZTS1F = linkonce_odr constant [11 x i8] c"1F\00\07\5A\6D\40\7C\72\54\9F", comdat
@_ZTI1F = linkonce_odr constant { ptr, ptr } { ptr null, ptr inttoptr (i64 add (i64 ptrtoint (ptr @_ZTS1F to i64), i64 -9220275812110958592) to ptr) }, comdat #0

declare dllimport ptr @F()

define ptr @main() {
  call ptr @F()
  ret ptr @_ZTI1F
}

attributes #0 = { "type-info" }

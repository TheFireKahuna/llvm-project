; REQUIRES: x86

;; Two images that each keep their own instance of a vague entity, which no
;; cell joins, split it: two protected copies. A link that sees both is an
;; error naming the entity, the two images and the fix, unless
;; -allow-vague-duplicate names the entity or, after '@', one of the images.
;; A DLL's link records its acknowledgements in its Images record, which a
;; later link inherits. A DLL's default copy that -Bsymbolic binds is a split
;; its author stated. A copy joins the owner only where its size, alignment
;; and guard agree. A copy that a DLL loaded late keeps protected splits from
;; any that binds before it loads, and one that it keeps preemptible counts
;; as a keeper, so that the executable's table names the owner.

; RUN: rm -rf %t && split-file %s %t && cd %t
; RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium \
; RUN:   %S/Inputs/fill-entry.s -o fill.obj
; RUN: llc -mtriple=x86_64-windows-itanium -filetype=obj p.ll -o p.obj
; RUN: llc -mtriple=x86_64-windows-itanium -filetype=obj r.ll -o r.obj
; RUN: llc -mtriple=x86_64-windows-itanium -filetype=obj s.ll -o s.obj
; RUN: llc -mtriple=x86_64-windows-gnu -filetype=obj f.ll -o f.obj
; RUN: llc -mtriple=x86_64-windows-itanium -filetype=obj q.ll -o q.obj
; RUN: llc -mtriple=x86_64-windows-itanium -filetype=obj main.ll -o main.obj
; RUN: lld-link -dll -noentry fill.obj p.obj -out:p.dll -implib:p.lib
; RUN: lld-link -dll -noentry fill.obj r.obj -out:r.dll -implib:r.lib
; RUN: lld-link -dll -noentry fill.obj s.obj -out:s.dll -implib:s.lib -Bsymbolic
; RUN: lld-link -dll -noentry fill.obj f.obj -out:f.dll -implib:f.lib \
; RUN:   -export:readf
; RUN: llc -mtriple=x86_64-windows-itanium -filetype=obj g.ll -o g.obj
; RUN: lld-link -dll -noentry fill.obj g.obj -out:g.dll -implib:g.lib

; DEFINE: %{exe} = lld-link -entry:main -subsystem:console main.obj p.lib
; RUN: not %{exe} r.lib -include:readr -out:main.exe 2>&1 | \
; RUN:   FileCheck --check-prefix=SPLIT %s
; RUN: %{exe} r.lib -include:readr -out:main.exe -allow-vague-duplicate:iv
; RUN: %{exe} r.lib -include:readr -out:main.exe -allow-vague-duplicate:@R.dll
; RUN: %{exe} s.lib -include:reads -out:main.exe
; RUN: not %{exe} f.lib -include:readf -out:main.exe 2>&1 | \
; RUN:   FileCheck --check-prefix=SHAPE -DDLL=f.dll %s
; RUN: not %{exe} g.lib -include:readg -out:main.exe 2>&1 | \
; RUN:   FileCheck --check-prefix=SHAPE -DDLL=g.dll %s
;; Each entity is reported once, by the first pair that splits it.
; RUN: not %{exe} f.lib g.lib -include:readf -include:readg -out:main.exe 2>&1 | \
; RUN:   FileCheck --check-prefix=SHAPE -DDLL=f.dll %s --implicit-check-not=error:
; RUN: not %{exe} r.lib f.lib -include:readr -include:readf -out:main.exe 2>&1 | \
; RUN:   FileCheck --check-prefix=SPLIT %s --implicit-check-not=error:

; RUN: llc -mtriple=x86_64-windows-itanium -filetype=obj t.ll -o t.obj
; RUN: llc -mtriple=x86_64-windows-itanium -filetype=obj u.ll -o u.obj
; RUN: llc -mtriple=x86_64-windows-itanium -filetype=obj late.ll -o late.obj
; RUN: lld-link -dll -noentry fill.obj t.obj -out:t.dll -implib:t.lib
; RUN: lld-link -dll -noentry fill.obj u.obj -out:u.dll -implib:u.lib
; DEFINE: %{late} = lld-link -entry:late -subsystem:console late.obj t.lib \
; DEFINE:   -include:readt -out:late.exe -alternatename:__delayLoadHelper2=late
; RUN: not %{late} r.lib -include:readr -delayload:r.dll 2>&1 | \
; RUN:   FileCheck --check-prefix=LATE %s
; RUN: %{late} r.lib -include:readr -delayload:r.dll -allow-vague-duplicate:iv
; RUN: %{late} u.lib -include:readu -delayload:u.dll
; RUN: llvm-readobj --coff-imports late.exe | FileCheck --check-prefix=OWNER %s

; LATE: error: iv is kept by t.dll and by r.dll, each with an instance of its own

; OWNER:      Name: t.dll
; OWNER-NEXT: ImportLookupTableRVA:
; OWNER-NEXT: ImportAddressTableRVA:
; OWNER-NEXT: Symbol: iv (0)
; OWNER-NEXT: Symbol: readt (1)

; SPLIT: error: iv is kept by p.dll and by r.dll, each with an instance of its own; link one against the other's import library, give it hidden visibility, or link the two jointly, or accept the split with -allow-vague-duplicate:iv

; SHAPE: error: iv is 8 bytes aligned to 8 in [[DLL]] and 4 bytes aligned to 4 in p.dll, whose copy the program shares, or has other bytes; the two are not one definition

; RUN: not lld-link -dll -noentry q.obj p.lib r.lib -out:q.dll 2>&1 | \
; RUN:   FileCheck --check-prefix=SPLIT %s
; RUN: lld-link -dll -noentry q.obj p.lib r.lib -out:q.dll -implib:q.lib \
; RUN:   -allow-vague-duplicate:iv
; RUN: llvm-readobj --coff-link-records q.lib | FileCheck --check-prefix=REC %s
; RUN: lld-link -entry:main -subsystem:console main.obj q.lib p.lib -out:main.exe \
; RUN:   -include:readq

; REC:      Name: q.dll
; REC-NEXT: Imports: p.dll
; REC-NEXT: Imports: r.dll
; REC-NEXT: AllowsDuplicate: iv

;--- p.ll
$iv = comdat any
@iv = linkonce_odr protected global i32 0, comdat

define dllexport i32 @readp() {
  %i = load i32, ptr @iv
  ret i32 %i
}

;--- r.ll
$iv = comdat any
@iv = linkonce_odr protected global i32 0, comdat

define dllexport i32 @readr() {
  %i = load i32, ptr @iv
  ret i32 %i
}

;--- s.ll
$iv = comdat any
@iv = linkonce_odr global i32 0, comdat

define dllexport i32 @reads() {
  %i = load i32, ptr @iv
  ret i32 %i
}

;--- f.ll
$iv = comdat any
@iv = linkonce_odr global i64 0, comdat

define ptr @readf() {
  ret ptr @iv
}

;--- g.ll
$iv = comdat any
@iv = linkonce_odr global i64 0, comdat

define dllexport ptr @readg() {
  ret ptr @iv
}

;--- t.ll
$iv = comdat any
@iv = linkonce_odr global i32 0, comdat

define dllexport ptr @readt() {
  ret ptr @iv
}

;--- u.ll
$iv = comdat any
@iv = linkonce_odr global i32 0, comdat

define dllexport ptr @readu() {
  ret ptr @iv
}

;--- late.ll
define void @late() {
  ret void
}

;--- q.ll
declare dllimport i32 @readp()
declare dllimport i32 @readr()

define dllexport i32 @readq() {
  %p = call i32 @readp()
  %r = call i32 @readr()
  %s = add i32 %p, %r
  ret i32 %s
}

;--- main.ll
declare dllimport i32 @readp()

define i32 @main() {
  %p = call i32 @readp()
  ret i32 %p
}

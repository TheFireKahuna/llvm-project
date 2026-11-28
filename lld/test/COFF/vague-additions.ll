; REQUIRES: x86

;; A DLL marks the vague entities that its baseline, the import library of a
;; release that it replaces, does not export, and warns of each: by default
;; the import library at its output path, which the link is about to
;; replace, or the one that -offers-baseline names. With none it marks
;; nothing and says so. A marked entity's offer entry and Images record say
;; it was added; a hidden one is no addition.
;;
;; Under -offers-epoch the DLL exports __llvm_offers$<digest> on an offered
;; export and states the digest in its record: the baseline's while the DLL
;; keeps nothing new, and otherwise a new one. A DLL that offers anything
;; imports the epoch of each image that it imports from; an executable does
;; not.

; RUN: rm -rf %t && split-file %s %t && cd %t
; RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium %S/Inputs/fill-entry.s -o fill.obj
; RUN: llc -mtriple=x86_64-windows-itanium -filetype=obj v1.ll -o v1.obj
; RUN: llc -mtriple=x86_64-windows-itanium -filetype=obj v2.ll -o v2.obj
; RUN: llc -mtriple=x86_64-windows-itanium -filetype=obj c.ll -o c.obj
; RUN: llc -mtriple=x86_64-windows-itanium -filetype=obj exe.ll -o exe.obj

; RUN: env LLD_IN_TEST=1 lld-link -dll -noentry fill.obj v1.obj -out:d.dll -offers-epoch -verbose 2>&1 | \
; RUN:   FileCheck --check-prefix=NONE %s
; RUN: cp d.lib base.lib
; RUN: llvm-readobj --coff-exports d.dll > out.txt
; RUN: llvm-readobj --coff-link-records d.lib >> out.txt
; RUN: env LLD_IN_TEST=1 lld-link -dll -noentry fill.obj v1.obj -out:d.dll -offers-epoch 2>&1 | count 0
; RUN: llvm-readobj --coff-exports d.dll >> out.txt
; RUN: env LLD_IN_TEST=1 lld-link -dll -noentry fill.obj v2.obj -out:d.dll -offers-epoch 2>&1 | \
; RUN:   FileCheck --check-prefix=WARN %s --implicit-check-not=warning
; RUN: llvm-readobj --coff-exports d.dll >> out.txt
; RUN: llvm-readobj --coff-link-records d.lib >> out.txt
; RUN: llvm-objdump -s -j .rdata d.dll >> out.txt
; RUN: lld-link -dll -noentry fill.obj c.obj base.lib -out:c.dll
; RUN: llvm-readobj --coff-imports c.dll >> out.txt
; RUN: lld-link -entry:main -subsystem:console exe.obj base.lib -out:e.exe
; RUN: llvm-readobj --coff-imports e.exe >> out.txt
; RUN: FileCheck --input-file=out.txt %s

; RUN: lld-link -dll -noentry fill.obj v2.obj -out:other.dll -offers-baseline:base.lib 2>&1 | \
; RUN:   FileCheck --check-prefix=WARN-OTHER %s --implicit-check-not=warning
; RUN: not lld-link -dll -noentry fill.obj v2.obj -out:e.dll -offers-baseline:none.lib 2>&1 | \
; RUN:   FileCheck --check-prefix=MISSING %s

; NONE: no import library at d.lib ({{.*}}), so no vague entity is marked

;; The first release's epoch, which the record states.
; CHECK:      Name: __llvm_offers$[[E1:[0-9a-f]{16}]]
; CHECK:      Name: a
; CHECK:      Epoch: 0x{{[0-9A-F]+}}

;; Relinked in place, it keeps nothing new and its epoch.
; CHECK:      Name: __llvm_offers$[[E1]]

;; The second keeps b, which its baseline does not export.
; WARN:       warning: d.dll offers b, which its baseline does not export
; WARN-OTHER: warning: other.dll offers b, which its baseline does not export

; CHECK:      Name: __llvm_offers$
; CHECK-NOT:  [[E1]]
; CHECK-SAME: {{$}}
; CHECK:      Entity {
; CHECK-NEXT: Name: a
; CHECK-NEXT: Visibility: Default
; CHECK-NEXT: Init: 1
; CHECK-NEXT: Binding: Preemptible
; CHECK-NEXT: Size: 4
; CHECK:      Entity {
; CHECK-NEXT: Name: b
; CHECK-NEXT: Visibility: Default
; CHECK-NEXT: Init: 1
; CHECK-NEXT: Binding: Preemptible
; CHECK-NEXT: Added: Yes
; CHECK:      Entity {
; CHECK-NEXT: Name: h
; CHECK-NEXT: Visibility: Hidden
; CHECK-NEXT: Init: 1
; CHECK-NEXT: Size: 4

;; b's offer entry: size 4, flags 0x1605 (added, indirect, preemptible, a
;; variable that its image initializes, default visibility), alignment 4.
; CHECK:      Contents of section .rdata:
; CHECK:      04000000 05160200

;; c.dll offers a and imports the first release's epoch; the executable
;; imports none.
; CHECK:      Name: d.dll
; CHECK:      Symbol: __llvm_offers$[[E1]] (0)
; CHECK:      Name: d.dll
; CHECK-NOT:  __llvm_offers
; CHECK:      Symbol: get

; MISSING: error: -offers-baseline: none.lib: {{.*}}

;--- v1.ll
$a = comdat any
@a = linkonce_odr global i32 1, comdat

define dllexport ptr @get() {
  ret ptr @a
}

;--- v2.ll
$a = comdat any
$b = comdat any
$h = comdat any
@a = linkonce_odr global i32 1, comdat
@b = linkonce_odr global i32 2, comdat
@h = linkonce_odr hidden global i32 3, comdat

define dllexport ptr @get() {
  ret ptr @a
}

define dllexport ptr @getb() {
  ret ptr @b
}

define dllexport ptr @geth() {
  ret ptr @h
}

;--- c.ll
$a = comdat any
@a = linkonce_odr global i32 1, comdat
declare dllimport ptr @get()

define dllexport ptr @c() {
  call ptr @get()
  ret ptr @a
}

;--- exe.ll
declare dllimport ptr @get()

define i32 @main() {
  call ptr @get()
  ret i32 0
}

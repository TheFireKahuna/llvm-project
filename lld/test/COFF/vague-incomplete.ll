; REQUIRES: x86, aarch64

;; A unit that sees class S incomplete refers to typeinfo for S through a weak
;; external whose default is a placeholder under a name of its own. Where a DLL
;; offers the complete object, the reference binds to its import, as a copy of
;; the object would, in an executable and in a DLL, which forwards its offer.
;; Where a DLL offers only the placeholder, the image's placeholder binds to
;; it, and so does __imp__ZTI1S, which the code loads: both are the address
;; table entry.

; RUN: rm -rf %t && split-file %s %t && cd %t

; DEFINE: %{t} = x86_64
; DEFINE: %{llc} = llc -mtriple=%{t}-windows-itanium -filetype=obj
; DEFINE: %{run} = \
; DEFINE:   llvm-mc -filetype=obj -triple=%{t}-windows-itanium \
; DEFINE:     %S/Inputs/fill-entry.s -o fill.obj && \
; DEFINE:   %{llc} s.ll -o s.obj && %{llc} inc.ll -o inc.obj && \
; DEFINE:   lld-link -dll -noentry fill.obj s.obj -out:s.dll -implib:s.lib && \
; DEFINE:   lld-link -dll -noentry fill.obj inc.obj -out:p.dll -implib:p.lib && \
; DEFINE:   lld-link -entry:main -subsystem:console inc.obj s.lib -out:e.exe && \
; DEFINE:   lld-link -dll -noentry fill.obj inc.obj s.lib -out:d.dll && \
; DEFINE:   lld-link -entry:main -subsystem:console inc.obj p.lib -out:q.exe && \
; DEFINE:   llvm-readobj --coff-imports e.exe | FileCheck --check-prefix=S %s && \
; DEFINE:   llvm-readobj --coff-imports --coff-exports d.dll | \
; DEFINE:     FileCheck --check-prefixes=S,FORWARD %s && \
; DEFINE:   llvm-readobj --coff-imports q.exe | \
; DEFINE:     FileCheck --check-prefix=PLACEHOLDER %s && \
; DEFINE:   llvm-objdump -d --no-show-raw-insn e.exe q.exe | \
; DEFINE:     FileCheck --check-prefix=LOAD-%{t} %s

; RUN: %{run}
; REDEFINE: %{t} = aarch64
; RUN: %{run}

; S:          Name: s.dll
; S-NEXT:     ImportLookupTableRVA:
; S-NEXT:     ImportAddressTableRVA: 0x2000
; S-NEXT:     Symbol: _ZTI1S (0)
; S-NOT:      Symbol:
; FORWARD:    Name: _ZTI1S
; FORWARD-NEXT: ForwardedTo: s.dll._ZTI1S

; PLACEHOLDER:      Name: p.dll
; PLACEHOLDER-NEXT: ImportLookupTableRVA:
; PLACEHOLDER-NEXT: ImportAddressTableRVA: 0x2000
; PLACEHOLDER-NEXT: Symbol: __llvm_incomplete$_ZTI1S (
; PLACEHOLDER-NOT:  Symbol:

;; get loads the address table entry in each.
; LOAD-x86_64-COUNT-2: movq 0xff9(%rip), %rax # 0x140002000
; LOAD-aarch64-COUNT-2: ldr x0, 0x140002000

;--- s.ll
$_ZTI1S = comdat any
$_ZTS1S = comdat any

@_ZTS1S = linkonce_odr constant [3 x i8] c"1S\00", comdat
@_ZTI1S = linkonce_odr constant { ptr, ptr } { ptr null, ptr @_ZTS1S }, comdat #0

define dllexport ptr @s() {
  ret ptr @_ZTI1S
}

attributes #0 = { "type-info" }

;--- inc.ll
$"__llvm_incomplete$_ZTI1S" = comdat any
$"__llvm_incomplete$_ZTS1S" = comdat any

@"__llvm_incomplete$_ZTI1S" = linkonce_odr dso_local constant { ptr, ptr } { ptr null, ptr @"__llvm_incomplete$_ZTS1S" }, comdat #0
@"__llvm_incomplete$_ZTS1S" = linkonce_odr dso_local constant [3 x i8] c"1S\00", comdat
@_ZTI1S = weak alias { ptr, ptr }, ptr @"__llvm_incomplete$_ZTI1S"

define dllexport ptr @get() {
  ret ptr @_ZTI1S
}

define void @main() {
  ret void
}

attributes #0 = { "type-info" }

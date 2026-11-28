; REQUIRES: x86, aarch64

;; A DLL's exported definition of a function that an overridable default has
;; a slot for is an offer to replace the default: the DLL exports it as
;; __ovr_offer$ and the name, privately, and its import library's link
;; records list it. The DLL's export of the name forwards to the default, and
;; its own calls and address-takes of the name go through the default's
;; import, as any image's do, so that they reach what the program chose. An
;; executable that does not replace the default takes the first offer of the
;; images that are loaded with it, in breadth-first order of their imports,
;; also of an image that it reaches only through another's import library:
;; its override table names the pointer it imports for the offer, which the
;; anchor's second mask says, so that every image's call and address of the
;; default is the offering DLL's function. Its own definition comes before
;; any offer. Bitcode offers and replaces as an object does, and LTO does
;; not bind a call of an offer that semantic interposition leaves
;; preemptible. A call of a default that forwards to an offered one reads
;; the offer's pointer, on both machines, and the default itself is not
;; imported. A default that LTO compiles stays the default that its check and
;; its address-takes name.

; RUN: rm -rf %t && split-file %s %t && cd %t

; DEFINE: %{t} = x86_64
; DEFINE: %{llc} = llc -mtriple=%{t}-windows-itanium -filetype=obj
; DEFINE: %{dll} = lld-link -dll -noentry
; DEFINE: %{link} = lld-link -entry:main -subsystem:console -out:a.exe
; DEFINE: %{image} = llvm-readobj --coff-imports --coff-exports
; DEFINE: %{table} = od -v -A x -t x4 -j 0x40 -N 0x50 a.exe > a.txt && \
; DEFINE:   llvm-objdump -s -j .ovra a.exe >> a.txt
; DEFINE: %{run} = \
; DEFINE:   %{llc} rt.ll -o rt.obj && %{llc} p.ll -o p.obj && \
; DEFINE:   %{llc} q.ll -o q.obj && %{llc} r.ll -o r.obj && \
; DEFINE:   %{llc} main.ll -o main.obj && %{llc} own.ll -o own.obj && \
; DEFINE:   %{llc} far.ll -o far.obj && %{llc} fill.ll -o fill.obj && \
; DEFINE:   %{llc} fwd.ll -o fwd.obj && \
; DEFINE:   rm -f fill.lib && llvm-ar rcs fill.lib fill.obj && \
; DEFINE:   opt -mtriple=%{t}-windows-itanium p.ll -o p.bc && \
; DEFINE:   opt -mtriple=%{t}-windows-itanium own.ll -o own.bc && \
; DEFINE:   %{dll} rt.obj fill.lib -out:rt.dll -implib:rt.lib && \
; DEFINE:   %{llc} crt.ll -o crt.obj && \
; DEFINE:   opt -mtriple=%{t}-windows-itanium def.ll -o def.bc && \
; DEFINE:   %{dll} def.bc crt.obj fill.lib -out:def.dll && \
; DEFINE:   %{dll} p.obj rt.lib fill.lib -out:p.dll -implib:p.lib && \
; DEFINE:   %{image} p.dll | FileCheck --check-prefix=OFFER %s && \
; DEFINE:   llvm-objdump -d --no-show-raw-insn p.dll \
; DEFINE:     | FileCheck --check-prefix=%{t} %s && \
; DEFINE:   llvm-readobj --coff-link-records p.lib \
; DEFINE:     | FileCheck --check-prefix=RECORD %s && \
; DEFINE:   llvm-nm p.lib | FileCheck --check-prefix=LIB %s \
; DEFINE:     --implicit-check-not=__ovr_offer && \
; DEFINE:   %{dll} p.bc rt.lib fill.lib -out:p.dll -implib:lto.lib && \
; DEFINE:   %{image} p.dll | FileCheck --check-prefix=OFFER %s && \
; DEFINE:   llvm-objdump -d --no-show-raw-insn p.dll \
; DEFINE:     | FileCheck --check-prefix=%{t} %s && \
; DEFINE:   %{dll} q.obj rt.lib -out:q.dll -implib:q.lib && \
; DEFINE:   %{dll} r.obj q.lib -out:r.dll -implib:r.lib && \
; DEFINE:   %{link} main.obj q.lib p.lib && %{table} && \
; DEFINE:   %{image} a.exe >> a.txt && \
; DEFINE:   FileCheck --input-file=a.txt --check-prefixes=TABLE,FIRST %s && \
; DEFINE:   %{link} main.obj q.lib p.lib -ignore-offers:p.dll && \
; DEFINE:   %{table} && %{image} a.exe >> a.txt && \
; DEFINE:   FileCheck --input-file=a.txt --check-prefixes=TABLE,SECOND %s && \
; DEFINE:   %{link} far.obj r.lib && %{table} && \
; DEFINE:   %{image} a.exe >> a.txt && \
; DEFINE:   FileCheck --input-file=a.txt --check-prefixes=TABLE,SECOND %s && \
; DEFINE:   %{link} fwd.obj p.lib rt.lib && \
; DEFINE:   llvm-readobj --coff-imports a.exe > a.txt && \
; DEFINE:   llvm-objdump -d --no-show-raw-insn a.exe >> a.txt && \
; DEFINE:   FileCheck --input-file=a.txt --check-prefixes=FWD,FWD-%{t} %s \
; DEFINE:     --implicit-check-not=_Znay && \
; DEFINE:   %{link} own.obj p.lib && %{table} && \
; DEFINE:   %{image} a.exe >> a.txt && \
; DEFINE:   FileCheck --input-file=a.txt --check-prefix=OWN %s \
; DEFINE:     --implicit-check-not=__ovr_offer && \
; DEFINE:   %{link} own.bc p.lib && %{table} && \
; DEFINE:   %{image} a.exe >> a.txt && \
; DEFINE:   FileCheck --input-file=a.txt --check-prefix=OWN %s \
; DEFINE:     --implicit-check-not=__ovr_offer

; RUN: %{run}
; REDEFINE: %{t} = aarch64
; RUN: %{run}

; OFFER:      Name: rt.dll
; OFFER:      Symbol: _Znwy
; OFFER:      Name: _Znwy
; OFFER-NEXT: ForwardedTo: rt.dll._Znwy
; OFFER:      Name: __ovr_offer$_Znwy
; OFFER-NEXT: RVA: 0x

; x86_64:      <call>:
; x86_64-NEXT: movl $0x1, %ecx
; x86_64-NEXT: jmp 0x[[#%x,THUNK:]]
; x86_64:      <addr>:
; x86_64-NEXT: movq {{.*}}(%rip), %rax
; x86_64:      [[#THUNK]]: jmpq *

; aarch64:      <call>:
; aarch64-NEXT: mov w0, #0x1
; aarch64-NEXT: b 0x[[#%x,THUNK:]]
; aarch64:      <addr>:
; aarch64-NEXT: ldr x0, 0x
; aarch64:      [[#THUNK]]: ldr x16,

; RECORD:      Name: p.dll
; RECORD-NEXT: Imports: rt.dll
; RECORD-NEXT: Entity {
; RECORD-NEXT:   Name: _Znwy
; RECORD-NEXT:   Visibility: Default
; RECORD-NEXT:   Kind: Override

; LIB: T __imp__Znwy

;; Both slots are tagged and both are offers. p.dll comes before q.dll, and
;; r.dll imports from q.dll.
; TABLE:  000040 c5e1a7f3 0000{{[0-9a-f]+}} c5e1a7f2 0000{{[0-9a-f]+}}
; TABLE:  Contents of section .ovra:
; TABLE-NEXT: d5c23a7b 01000800 03000000 {{[0-9a-f]+}}
; TABLE-NEXT: 00000000 03000000
; FIRST:  Name: q.dll
; FIRST:  Symbol: __ovr_offer$_Znay
; FIRST:  Name: p.dll
; FIRST:  Symbol: __ovr_offer$_Znwy
; SECOND:     Name: q.dll
; SECOND-DAG: Symbol: __ovr_offer$_Znay
; SECOND-DAG: Symbol: __ovr_offer$_Znwy

; FWD:      Name: p.dll
; FWD-NEXT: ImportLookupTableRVA:
; FWD-NEXT: ImportAddressTableRVA: 0x2000
; FWD-NEXT: Symbol: __ovr_offer$_Znwy (0)
; FWD-NEXT: Symbol: call (3)
; FWD-x86_64:      movq {{.*}} # 0x140002008
; FWD-x86_64:      movq {{.*}} # 0x140002000
; FWD-x86_64-NEXT: callq *%rax
; FWD-aarch64:      ldr x8, 0x140002008
; FWD-aarch64:      ldr x8, 0x140002000
; FWD-aarch64-NEXT: blr x8

; OWN: 000040 c5e1a7f3 0000{{[0-9a-f]+}} c5e1a7f2 0000{{[0-9a-f]+}}
; OWN: Symbol: call

;--- rt.ll
define weak dllexport ptr @_Znwy(i64 %n) !overridable !0 {
  ret ptr null
}

define weak dllexport ptr @_Znay(i64 %n) !overridable !1 {
  ret ptr null
}

;; What a CRT defines, which the defaults' tails read.
@__guard_dispatch_icall_fptr = constant ptr null
@__guard_check_icall_fptr = constant ptr null

!0 = !{i32 0}
!1 = !{i32 1, i32 0}

;--- p.ll
define dllexport ptr @_Znwy(i64 %n) !overridable !0 {
  ret ptr inttoptr (i64 1 to ptr)
}

define dllexport ptr @call() {
  %p = tail call ptr @_Znwy(i64 1)
  ret ptr %p
}

define dllexport ptr @addr() {
  ret ptr @_Znwy
}

!llvm.module.flags = !{!1}
!0 = !{}
!1 = !{i32 1, !"SemanticInterposition", i32 1}

;--- q.ll
define dllexport ptr @_Znwy(i64 %n) !overridable !0 {
  ret ptr inttoptr (i64 2 to ptr)
}

define dllexport ptr @_Znay(i64 %n) !overridable !0 {
  ret ptr inttoptr (i64 3 to ptr)
}

define dllexport void @q() {
  ret void
}

!0 = !{}

;--- r.ll
declare dllimport void @q()

define dllexport void @r() {
  call void @q()
  ret void
}

;--- main.ll
declare dllimport ptr @call()
declare dllimport void @q()

define i32 @main() {
  call ptr @call()
  call void @q()
  ret i32 0
}

;--- far.ll
declare dllimport void @r()

define i32 @main() {
  call void @r()
  ret i32 0
}

;--- fwd.ll
declare dllimport ptr @call()
declare dllimport ptr @_Znay(i64)

define i32 @main() {
  call ptr @call()
  %p = call ptr @_Znay(i64 1)
  ret i32 0
}

;--- fill.ll
$__llvm_fill_dll = comdat any
$__llvm_fill_true = comdat any

@__llvm_fill_list = external dso_local global i8
declare dso_local void @__llvm_fill_target(ptr)

define void @__llvm_fill_dll() comdat {
  call void @__llvm_fill_target(ptr @__llvm_fill_list)
  ret void
}

define i32 @__llvm_fill_true() comdat {
  ret i32 1
}

;--- crt.ll
@__guard_dispatch_icall_fptr = constant ptr null
@__guard_check_icall_fptr = constant ptr null

;--- def.ll
define weak ptr @_Znwy(i64 %n) !overridable !0 {
  ret ptr null
}

define dllexport ptr @call() {
  %p = call ptr @_Znwy(i64 1)
  ret ptr %p
}

define dllexport ptr @addr() {
  ret ptr @_Znwy
}

!0 = !{i32 0}

;--- own.ll
declare dllimport ptr @call()

define ptr @_Znwy(i64 %n) !overridable !0 {
  ret ptr inttoptr (i64 4 to ptr)
}

define i32 @main() {
  call ptr @call()
  ret i32 0
}

!0 = !{}

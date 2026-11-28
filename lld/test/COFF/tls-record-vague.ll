; REQUIRES: x86

;; Images share a thread-local variable with vague linkage by its record. A
;; DLL that keeps one offers the record, never the variable, and the record of
;; the variable's guard with it, and the function that initializes a variable
;; with dynamic initialization, which a unit that only declares it calls. An image that defines the variable and reads
;; the import library takes the DLL's records for its own: the variable and
;; its guard leave the image's thread-local data, and the code that
;; initializes the variable in a thread stays and reaches the DLL's instance
;; and guard through the records, as every other access does. A DLL reaches
;; one that it keeps preemptible through the cell of its record, as it does any
;; vague entity, and an image that takes the offer through its import's entry,
;; which the fill binds alike; an executable that keeps none imports the
;; owner's record. -Bsymbolic binds a DLL's own, but not a unique one's.
;; Bitcode is compiled first, and gives way like an object.

; RUN: rm -rf %t && split-file %s %t && cd %t
; RUN: llc -filetype=obj a.ll -o a.obj && opt a.ll -o a.bc
; RUN: llvm-mc -triple=x86_64-windows-itanium -filetype=obj sup.s -o sup.obj
; RUN: llvm-mc -triple=x86_64-windows-itanium -filetype=obj \
; RUN:   %S/Inputs/fill-entry.s -o fill.obj
; RUN: lld-link -dll -noentry a.obj sup.obj fill.obj -out:a.dll -implib:a.lib \
; RUN:   -debug:symtab
; RUN: llvm-readobj --coff-exports a.dll | FileCheck %s --check-prefix=OFFERS \
; RUN:   --implicit-check-not=Name:
; RUN: lld-link -dll -noentry a.bc sup.obj fill.obj -out:lto.dll -export:use
; RUN: llvm-readobj --coff-exports lto.dll | FileCheck %s --check-prefixes=OFFERS,USE \
; RUN:   --implicit-check-not=Name:
; RUN: llvm-objdump -d --no-show-raw-insn a.dll | FileCheck %s --check-prefix=CELL

;; The code that LTO compiles for an executable that keeps the variable reads
;; it as it reads any variable of the image, and needs no record.
; RUN: lld-link -entry:use -subsystem:console a.bc sup.obj -out:lto.exe \
; RUN:   -debug:symtab
; RUN: llvm-objdump -d --no-show-raw-insn lto.exe | FileCheck %s --check-prefix=OWN
; OFFERS:      Name: _ZGV2tv$tls
; OFFERS-NEXT: RVA:
; OFFERS:      Name: _ZTH2tv
; OFFERS-NEXT: RVA:
; OFFERS:      Name: tu$tls
; OFFERS-NEXT: RVA:
; OFFERS:      Name: tv$tls
; OFFERS-NEXT: RVA:
; USE:         Name: use
; CELL:        <use>:
; CELL:        movq {{.*}}(%rip), %rax # 0x180003008 <__imp_tv$tls>
; OWN:         <use>:
; OWN-NOT:     movq (%r{{[a-z0-9]+}}), %r
; OWN:         retq

; DEFINE: %{kind} =
; DEFINE: %{in} = a.obj
; DEFINE: %{link} = lld-link -dll -noentry %{in} sup.obj fill.obj a.lib -out:b.dll \
; DEFINE:   -lldmap:b.map -debug:symtab %{kind}
; DEFINE: %{exports} = llvm-readobj --coff-exports --coff-imports b.dll | \
; DEFINE:   FileCheck %s --implicit-check-not=Symbol: --check-prefixes=CHECK

; RUN: %{link} && %{exports},GONE
; RUN: FileCheck %s --check-prefixes=INIT,MAP --input-file=b.map
; RUN: llvm-objdump -d --no-show-raw-insn b.dll | FileCheck %s --check-prefix=CODE
; REDEFINE: %{kind} = -opt:noref -debug
; RUN: %{link} && %{exports},GONE
; REDEFINE: %{in} = a.bc
; REDEFINE: %{kind} = -export:use
; RUN: %{link} && %{exports},GONE
; RUN: FileCheck %s --check-prefix=MAP --input-file=b.map
; RUN: llvm-objdump -d --no-show-raw-insn b.dll | FileCheck %s --check-prefix=LTO
; RUN: opt -module-summary a.ll -o thin.bc
; REDEFINE: %{in} = thin.bc
; RUN: %{link} && %{exports},GONE
; RUN: llvm-objdump -d --no-show-raw-insn b.dll | FileCheck %s --check-prefix=LTO
; REDEFINE: %{in} = a.obj
; REDEFINE: %{kind} = -Bsymbolic
; RUN: %{link} && %{exports},BOUND
;; A DLL whose offers the image ignores keeps an instance of its own, which
;; its cell binds to the program's as the image starts. An executable that
;; keeps none imports the record of the owner, the first breadth-first.
; REDEFINE: %{kind} = -ignore-offers:a.dll -export:use
; RUN: %{link} && %{exports},KEPT
; RUN: llvm-mc -triple=x86_64-windows-itanium -filetype=obj main.s -o main.obj
; RUN: lld-link -entry:main -subsystem:console main.obj a.lib b.lib -out:main.exe
; RUN: llvm-readobj --coff-imports main.exe | FileCheck %s --check-prefix=OWNER
; OWNER:      Name: a.dll
; OWNER-NEXT: ImportLookupTableRVA:
; OWNER-NEXT: ImportAddressTableRVA:
; OWNER-NEXT: Symbol: _ZTH2tv
; OWNER-NEXT: Symbol: tu$tls
; OWNER-NEXT: Symbol: tv$tls
; OWNER-NEXT: }
; OWNER-NEXT: Import {
; OWNER-NEXT: Name: b.dll

; GONE:        Symbol: _ZGV2tv$tls (
; GONE:        Symbol: tu$tls (
; GONE:        Symbol: tv$tls (
; BOUND:       Symbol: tu$tls (
; CHECK:       Name: _ZGV2tv$tls
; GONE-NEXT:   ForwardedTo: a.dll._ZGV2tv$tls
; BOUND-NEXT:  RVA:
; KEPT-NEXT:   RVA:
; CHECK:       Name: _ZTH2tv
; GONE-NEXT:   ForwardedTo: a.dll._ZTH2tv
; BOUND-NEXT:  RVA:
; KEPT-NEXT:   RVA:
; CHECK:       Name: tu$tls
; GONE-NEXT:   ForwardedTo: a.dll.tu$tls
; BOUND-NEXT:  ForwardedTo: a.dll.tu$tls
; KEPT-NEXT:   RVA:
; CHECK:       Name: tv$tls
; GONE-NEXT:   ForwardedTo: a.dll.tv$tls
; BOUND-NEXT:  RVA:
; KEPT-NEXT:   RVA:

;; The initialization function is the image's, and tests the DLL's guard,
;; whose offset it reads from the variable's record.
;; Of the object's thread-local data, only the byte that no image shares is
;; left.
; INIT:     thread-local initialization routine for tv
; MAP:      {{.*}}:(.tls$)
; MAP-NEXT: __tls_set
; MAP-NEXT: sup.obj:(.tls$ZZZ)
; CODE:      <init>:
; CODE-NEXT: movq {{.*}}(%rip), %rax # {{.*}} <__imp_tv$tls>
; CODE-NEXT: movq (%rax), %rcx
; CODE-NEXT: movslq (%rcx), %rcx
; CODE-NEXT: movq %gs:0x58, %rdx
; CODE-NEXT: movq (%rdx,%rcx,8), %rcx
; CODE-NEXT: movl 0xc(%rax), %edx
; CODE-NEXT: cmpb $0x0, (%rcx,%rdx)

;; So does the code that LTO compiles for such an image.
; LTO:      <use>:
; LTO-NEXT: movq {{.*}}(%rip), %r{{.*}} # {{.*}} <__imp_tv$tls>

;; Another producer's object reaches its image's instance of a thread-local
;; variable, which the image keeps, in either order, and offers by the record
;; that an object for Windows Itanium states, protected, so that a.dll's cell
;; binds to it. Alone, it offers nothing: a thread-local variable has no
;; address to export.
; RUN: llvm-mc -triple=x86_64-windows-msvc -filetype=obj foreign.s -o foreign.obj
; RUN: lld-link -dll -noentry a.obj foreign.obj sup.obj fill.obj a.lib -out:f.dll \
; RUN:   -export:fuse
; RUN: llvm-readobj --coff-exports f.dll | FileCheck %s --check-prefix=FOREIGN
; RUN: lld-link -dll -noentry foreign.obj a.obj sup.obj fill.obj a.lib -out:f.dll \
; RUN:   -export:fuse
; RUN: llvm-readobj --coff-exports f.dll | FileCheck %s --check-prefix=FOREIGN
; RUN: lld-link -dll -noentry foreign.obj sup.obj -out:g.dll -export:fuse
; RUN: llvm-readobj --coff-exports g.dll | FileCheck %s --check-prefix=ALONE \
; RUN:   --implicit-check-not=Name:
; FOREIGN:      Name: tu$tls
; FOREIGN-NEXT: RVA:
; ALONE:        Name: fuse

;--- a.ll
target triple = "x86_64-unknown-windows-itanium"

$tv = comdat any
$tu = comdat any
$__tls_set = comdat any

@tv = linkonce_odr thread_local global i32 0, comdat, !tls_record !0 #0
@_ZGV2tv = linkonce_odr thread_local global i8 0, comdat($tv), !tls_record !0
@tu = linkonce_odr thread_local global i32 5, comdat, !tls_record !1 #1
@__tls_set = linkonce_odr hidden thread_local constant i8 1, comdat
@llvm.compiler.used = appending global [1 x ptr] [ptr @__tls_set], section "llvm.metadata"

@_ZTH2tv = linkonce_odr alias void (), ptr @init

define internal void @init() comdat($tv) {
  %g = load i8, ptr @_ZGV2tv
  %set = icmp eq i8 %g, 0
  br i1 %set, label %init, label %done

init:
  store i8 1, ptr @_ZGV2tv
  store i32 7, ptr @tv
  br label %done

done:
  ret void
}

define i32 @use() {
  call void @init()
  %a = load i32, ptr @tv
  %b = load i32, ptr @tu
  %c = add i32 %a, %b
  ret i32 %c
}

attributes #0 = { "dynamic-init"="thread-local" }
attributes #1 = { "gnu-unique" }

!0 = !{ptr @_ZGV2tv}
!1 = !{ptr @__tls_set}

;--- sup.s
        .section .tls,"dw"
        .globl  _tls_start
_tls_start:
        .quad   0
        .section .tls$ZZZ,"dw"
        .globl  _tls_end
_tls_end:
        .quad   0
        .data
        .globl  _tls_index
_tls_index:
        .long   0
        .section .rdata,"dr"
        .globl  _tls_used
_tls_used:
        .quad   _tls_start
        .quad   _tls_end
        .quad   _tls_index
        .quad   0
        .long   0
        .long   0
;--- main.s
        .text
        .globl  main
main:
        callq   *__imp__ZTH2tv(%rip)
        jmp     use
;--- foreign.s
        .section .tls$,"dw",discard,tu
        .globl  tu
        .p2align 2
tu:
        .long   5
        .text
        .globl  fuse
fuse:
        movl    _tls_index(%rip), %eax
        movq    %gs:88, %rcx
        movq    (%rcx,%rax,8), %rax
        movl    tu@SECREL32(%rax), %eax
        retq
.section .text,"xr",one_only,__llvm_fill_tls
.globl __llvm_fill_tls
__llvm_fill_tls:
  ret
.section .CRT$XLAF,"dr",associative,__llvm_fill_tls
  .quad __llvm_fill_tls

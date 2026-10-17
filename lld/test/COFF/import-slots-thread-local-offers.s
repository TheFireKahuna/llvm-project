# REQUIRES: x86
## Under -import-slots, a DLL offers a vague thread-local variable that it
## keeps through the variable's record, with its guard's, which the DLL writes,
## and never the variable or its guard. An image that imports from the DLL
## takes the offer: its own copy and its guard leave its TLS template, and its
## references to their records reach the DLL's. An image that does not take
## the offer keeps its copy, and reaches the record it writes; the entity is
## then named in the report of duplicates as its record is. A copy in bitcode
## is preempted before LTO.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium vague.s -o vague.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium dll.s -o dll.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium main.s -o main.obj
# RUN: lld-link -import-slots -dll -noentry dll.obj vague.obj -out:dll.dll
# RUN: llvm-readobj --coff-exports dll.dll | FileCheck --check-prefix=OFFERS %s

# OFFERS-NOT: Name: x{{$}}
# OFFERS-NOT: Name: _ZGV1x{{$}}
# OFFERS:     Name: _ZGV1x$tls
# OFFERS:     Name: fx
# OFFERS:     Name: x$tls
# OFFERS-NOT: Name: x{{$}}
# OFFERS-NOT: Name: _ZGV1x{{$}}

# RUN: lld-link -import-slots -entry:main -subsystem:console main.obj \
# RUN:   vague.obj dll.lib -out:main.exe
# RUN: llvm-readobj --sections --coff-imports main.exe | \
# RUN:   FileCheck --check-prefix=TAKEN %s

# TAKEN-NOT:  Name: .tls
# TAKEN:      Name: dll.dll
# TAKEN:      Symbol: _ZGV1x$tls
# TAKEN-NEXT: Symbol: fx
# TAKEN-NEXT: Symbol: x$tls

# RUN: llvm-as main.ll -o main.bc
# RUN: lld-link -import-slots -entry:main -subsystem:console main.bc \
# RUN:   dll.lib -out:lto.exe
# RUN: llvm-readobj --sections --coff-imports lto.exe | \
# RUN:   FileCheck --check-prefix=LTO %s

# LTO-NOT:  Name: .tls
# LTO:      Name: dll.dll
# LTO:      Symbol: x$tls

# RUN: not lld-link -import-slots -entry:main -subsystem:console main.obj \
# RUN:   vague.obj dll.lib -ignore-offers:dll.dll -out:kept.exe 2>&1 | \
# RUN:   FileCheck --check-prefix=DUP %s
# DUP: error: kept.exe keeps its own copy of x$tls, which dll.dll, which it imports from, defines; take dll.dll's offer

# RUN: lld-link -import-slots -entry:main -subsystem:console main.obj \
# RUN:   vague.obj dll.lib -ignore-offers:dll.dll \
# RUN:   -allow-vague-duplicates:dll.dll -out:kept.exe
# RUN: llvm-readobj --sections --coff-imports kept.exe > kept.txt
# RUN: llvm-objdump -d kept.exe >> kept.txt
# RUN: FileCheck --check-prefix=KEPT --input-file=kept.txt %s

# KEPT:      Name: .tls
# KEPT-NEXT: VirtualSize: 0x10
# KEPT:      Name: dll.dll
# KEPT-NEXT: ImportLookupTableRVA:
# KEPT-NEXT: ImportAddressTableRVA:
# KEPT-NEXT: Symbol: fx
# KEPT-NEXT: }
# KEPT:      <.text>:
# KEPT-NEXT: leaq
# KEPT-NEXT: leaq

#--- vague.s
## An inline thread-local variable and its guard, as clang emits them.
  .linkbindings
  .section .tls$,"dw",discard,x
  .globl x
  .p2align 2
x:
  .linkbinding x, default
  .long 1

  .section .tls$,"dw",discard,_ZGV1x
  .globl _ZGV1x
  .p2align 3
_ZGV1x:
  .linkbinding _ZGV1x, default
  .quad 0

  .data
  .globl _tls_index
_tls_index:
  .long 0

#--- dll.s
  .text
  .globl fx
fx:
  movq "__imp_x$tls"(%rip), %rax
  movq "__imp__ZGV1x$tls"(%rip), %rcx
  retq

  .section .drectve,"yni"
  .ascii " -export:fx"

#--- main.s
  .text
  .globl main
main:
  movq "__imp_x$tls"(%rip), %rax
  movq "__imp__ZGV1x$tls"(%rip), %rcx
  callq *__imp_fx(%rip)
  retq

#--- main.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-windows-itanium"

$x = comdat any

@x = linkonce_odr thread_local global i32 1, comdat

define i32 @main() {
  %p = call ptr @llvm.threadlocal.address.p0(ptr @x)
  %v = load i32, ptr %p
  ret i32 %v
}

declare ptr @llvm.threadlocal.address.p0(ptr)

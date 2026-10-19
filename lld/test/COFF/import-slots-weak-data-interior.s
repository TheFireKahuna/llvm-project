# REQUIRES: x86
## Under -import-slots, an image's static data that holds an address inside
## COMDAT data a DLL's instance preempts binds to the name the DLL exports for
## that address, which the loader writes whole, though the image's own copy
## names no such address.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium a.s -o a.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium b.s -o b.obj
# RUN: lld-link -import-slots -dll -noentry a.obj -out:a.dll
# RUN: lld-link -import-slots -dll -noentry b.obj a.lib -out:b.dll
# RUN: llvm-readobj --coff-imports b.dll | FileCheck %s
# CHECK:      Name: a.dll
# CHECK-NEXT: ImportLookupTableRVA:
# CHECK-NEXT: ImportAddressTableRVA:
# CHECK-NEXT: Symbol: w$so4 (2)
# CHECK-NEXT: }

#--- a.s
  .linkbindings
  .section .data,"dw",discard,w
  .globl w, w$so4
w:
  .linkbinding w, default
  .long 7
w$so4:
  .long 8

  .text
  .globl af
af:
  movq __imp_w(%rip), %rax
  retq

  .section .drectve,"yni"
  .ascii " -export:af"

#--- b.s
  .linkbindings
  .section .rdata,"dr"
  .globl p
p:
  .quad w+4

  .section .data,"dw",discard,w
  .globl w
w:
  .linkbinding w, default
  .long 7, 8

  .section .drectve,"yni"
  .ascii " -export:p,DATA"

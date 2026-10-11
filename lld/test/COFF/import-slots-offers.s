# REQUIRES: x86
## Under -import-slots, a link that writes an import library offers each
## definition with default visibility in a COMDAT that the image keeps and
## another image may observe, as an ELF shared object's dynamic symbol table
## gives it: data, with the other external symbols of its group and its guard,
## and functions whose addresses are significant. A definition its object does
## not list as having default visibility is hidden and not offered. What the
## image takes from a DLL that offers it is offered as a forwarder to that DLL,
## and the image's import library names the DLL in its place, so that an
## importer binds to the DLL that holds it.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium a.s -o a.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium b.s -o b.obj

# RUN: lld-link -import-slots -dll -noentry a.obj -out:a.dll
# RUN: llvm-readobj --coff-exports a.dll | FileCheck --check-prefix=OFFERS %s
# OFFERS-NOT: Name: hid
# OFFERS-NOT: Name: nf
# OFFERS-NOT: Name: strong
# OFFERS:     Name: _ZGV2iv
# OFFERS-NOT: Name: hid
# OFFERS:     Name: f
# OFFERS-NOT: Name: hid
# OFFERS:     Name: iv
# OFFERS-NEXT: RVA:
# OFFERS-NEXT: }
# OFFERS-NEXT: Export {
# OFFERS-NEXT:  Ordinal:
# OFFERS-NEXT:  Name: iv$so4
# OFFERS-NOT: Name: hid
# OFFERS-NOT: Name: nf

## An executable offers nothing unless it writes an import library, and a link
## without -import-slots offers nothing.
# RUN: lld-link -import-slots -entry:main -subsystem:console a.obj -out:a.exe
# RUN: llvm-readobj --coff-exports a.exe | FileCheck --check-prefix=NONE %s
# RUN: lld-link -import-slots -entry:main -subsystem:console a.obj \
# RUN:   -implib:host.lib -out:host.exe
# RUN: llvm-readobj --coff-exports host.exe | \
# RUN:   FileCheck --check-prefix=HOST %s
# RUN: lld-link -dll -noentry a.obj -out:plain.dll
# RUN: llvm-readobj --coff-exports plain.dll | FileCheck --check-prefix=NONE %s
# NONE-NOT: Name: {{iv|f}}
# HOST: Name: iv

## A DLL that imports iv and its guard from a.dll forwards them, and its
## import library names a.dll for them.
# RUN: lld-link -import-slots -dll -noentry b.obj a.lib -out:b.dll \
# RUN:   -implib:b.lib -verbose 2>&1 | FileCheck --check-prefix=PREEMPT %s
# RUN: llvm-readobj --coff-exports b.dll | FileCheck --check-prefix=FWD %s
# RUN: llvm-nm b.lib | FileCheck --check-prefix=LIB %s
# PREEMPT: Preempting iv in b.obj with its instance in a.dll
# FWD:      Name: iv
# FWD-NEXT: ForwardedTo: a.iv
# LIB:      a.dll:
# LIB-NEXT: {{0+}} D __imp_iv
# LIB-NOT:  b.dll:{{$}}
# LIB-NOT:  __imp_iv

#--- a.s
  .linkbindings
  .section .data,"dw",discard,iv
  .globl iv
  .p2align 3
iv:
  .linkbinding iv, default
  .quad 1
  .globl "iv$so4"
  .set "iv$so4", iv+4

  .section .bss,"bw",discard,_ZGV2iv
  .globl _ZGV2iv
_ZGV2iv:
  .linkbinding _ZGV2iv, default
  .quad 0

  .section .data,"dw",discard,hid
  .globl hid
hid:
  .long 2

  .section .text,"xr",discard,f
  .globl f
f:
  .linkbinding f, default
  retq

  .section .text,"xr",discard,nf
  .globl nf
nf:
  .linkbinding nf, default
  retq

  .data
  .globl strong
strong:
  .quad iv
  .quad _ZGV2iv
  .quad hid
  .quad f

  .text
  .globl main
main:
  callq nf
  retq

  .addrsig
  .addrsig_sym f

#--- b.s
  .linkbindings
  .section .data,"dw",discard,iv
  .globl iv
  .p2align 3
iv:
  .linkbinding iv, default
  .quad 1

  .data
  .globl bstrong
bstrong:
  .quad iv

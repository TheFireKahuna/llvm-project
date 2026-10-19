# REQUIRES: x86
## Under -import-slots, a link that writes an import library offers each
## definition with default visibility in a COMDAT that the image keeps and
## another image may observe, as an ELF shared object's dynamic symbol table
## gives it: data, with the other external symbols of its group and its guard,
## and functions whose addresses are significant. A definition its object does
## not list as having default visibility is hidden and not offered, nor is
## another name at its address that the object does not list; a name inside it
## has its visibility. What the image takes from a DLL that offers it is
## offered as a forwarder to that DLL, the whole group where it takes one name
## of it, and the image's import library names the DLL in its place, so that
## an importer binds to the DLL that holds it.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium a.s -o a.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium b.s -o b.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium c.s -o c.obj

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
# OFFERS:     Name: iva
# OFFERS-NOT: Name: hid
# OFFERS-NOT: Name: ivh
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

## A DLL that imports iv from a.dll forwards it, and its guard, whose copy
## nothing in the DLL uses, and its import library names a.dll for them.
# RUN: lld-link -import-slots -dll -noentry b.obj a.lib -out:b.dll \
# RUN:   -implib:b.lib -verbose 2>&1 | FileCheck --check-prefix=PREEMPT %s
# RUN: llvm-readobj --coff-exports b.dll | FileCheck --check-prefix=FWD %s
# RUN: llvm-nm b.lib | FileCheck --check-prefix=LIB %s
# PREEMPT: Preempting iv in b.obj with its instance in a.dll
# FWD:      Name: _ZGV2iv
# FWD-NEXT: ForwardedTo: a._ZGV2iv
# FWD:      Name: iv
# FWD-NEXT: ForwardedTo: a.iv
# LIB:      a.dll:
# LIB-NEXT: {{0+}} D __imp__ZGV2iv
# LIB:      a.dll:
# LIB-NEXT: {{0+}} D __imp_iv
# LIB-NOT:  __imp_iv

## A DLL whose copy is discarded and that takes one name of the group from
## a.dll forwards the group's other names too.
# RUN: lld-link -import-slots -dll -noentry c.obj a.lib -out:c.dll
# RUN: llvm-readobj --coff-exports c.dll | FileCheck --check-prefix=GROUP %s
# GROUP:      Name: iv
# GROUP-NEXT: ForwardedTo: a.iv
# GROUP:      Name: iv$so4
# GROUP-NEXT: ForwardedTo: a.iv$so4
# GROUP:      Name: iva
# GROUP-NEXT: ForwardedTo: a.iva

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
  .globl iva
  .linkbinding iva, default
  .set iva, iv
  .globl ivh
  .set ivh, iv

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

  .section .bss,"bw",discard,_ZGV2iv
  .globl _ZGV2iv
_ZGV2iv:
  .linkbinding _ZGV2iv, default
  .quad 0

  .data
  .globl bstrong
bstrong:
  .quad iv

#--- c.s
  .linkbindings
  .section .data,"dw",discard,iv
  .globl iv
  .p2align 3
iv:
  .linkbinding iv, default
  .quad 1
  .globl "iv$so4"
  .set "iv$so4", iv+4
  .globl iva
  .linkbinding iva, default
  .set iva, iv

  .text
  .globl get
get:
  movq __imp_iva(%rip), %rax
  retq

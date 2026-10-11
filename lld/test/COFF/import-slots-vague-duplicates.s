# REQUIRES: x86
## Under -import-slots, a DLL's import library records the vague entities with
## default visibility that the DLL keeps, the DLLs it imports from and their
## records, and a link reports two images of the load closure of what it
## imports that both keep an entity, where ELF would have unified them: true
## siblings, or an image that keeps its copy while importing from one that
## defines the entity. An entity or a DLL can be allowed to keep its copies.
## A DLL's link can compare what it keeps with its previous release, and
## export an epoch that the DLLs offering what they import from it import.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium a.s -o a.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium b.s -o b.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium c.s -o c.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium old.s -o old.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium main3.s -o main3.obj

# RUN: lld-link -import-slots -dll -noentry a.obj -export:fa -out:a.dll
# RUN: lld-link -import-slots -dll -noentry b.obj -export:fb -out:b.dll
# RUN: llvm-nm a.lib | FileCheck --check-prefix=RECORD %s
# RECORD: __llvm_vague$a.dll

## Siblings that no import path joins.
# RUN: not lld-link -import-slots -entry:main -subsystem:console main.obj \
# RUN:   a.lib b.lib -out:m.exe 2>&1 | FileCheck --check-prefix=SIBLINGS %s
# SIBLINGS: error: _ZN1n1gE is defined by both a.dll and b.dll, neither of which imports from the other; link one against the other's import library, give it hidden visibility, or link both jointly
# RUN: lld-link -import-slots -entry:main -subsystem:console main.obj a.lib \
# RUN:   b.lib -allow-vague-duplicate:_ZN1n1gE -out:m.exe
# RUN: lld-link -import-slots -entry:main -subsystem:console main.obj a.lib \
# RUN:   b.lib -allow-vague-duplicates:B.dll -out:m.exe

## A DLL linked against a release of a.dll that did not define the entity
## keeps its copy; a later link against both reports it.
# RUN: mkdir old && lld-link -import-slots -dll -noentry old.obj -export:fa \
# RUN:   -out:old/a.dll
# RUN: lld-link -import-slots -dll -noentry c.obj -export:fc old/a.lib \
# RUN:   -out:c.dll
# RUN: not lld-link -import-slots -entry:main -subsystem:console main3.obj \
# RUN:   a.lib b.lib c.lib -allow-vague-duplicates:b.dll -out:m.exe 2>&1 | \
# RUN:   FileCheck --check-prefix=STALE %s
# STALE: error: c.dll keeps its own copy of _ZN1n1gE, which a.dll, which it imports from, defines; relink c.dll

## The additions gate, and the epoch.
# RUN: lld-link -import-slots -dll -noentry a.obj -export:fa -out:a2.dll \
# RUN:   -implib:a2.lib -offers-baseline:old/a.lib -offers-epoch 2>&1 | \
# RUN:   FileCheck --check-prefix=GATE %s
# RUN: llvm-readobj --coff-exports a2.dll | FileCheck --check-prefix=EPOCH %s
# GATE: warning: a2.dll newly defines _ZN1n1gE; a DLL linked against its previous import library keeps its own copy until relinked
# EPOCH: Name: __llvm_offers${{[0-9a-f]+$}}
# RUN: lld-link -import-slots -dll -noentry c.obj -export:fc a2.lib \
# RUN:   -out:d.dll
# RUN: llvm-readobj --coff-imports d.dll | FileCheck --check-prefix=IMPORT %s
# IMPORT: Symbol: __llvm_offers$

#--- a.s
  .linkbindings
  .section .data,"dw",discard,_ZN1n1gE
  .globl _ZN1n1gE
_ZN1n1gE:
  .linkbinding _ZN1n1gE, default
  .long 1

  .text
  .globl fa
fa:
  leaq _ZN1n1gE(%rip), %rax
  retq

#--- old.s
  .text
  .globl fa
fa:
  retq

#--- b.s
  .linkbindings
  .section .data,"dw",discard,_ZN1n1gE
  .globl _ZN1n1gE
_ZN1n1gE:
  .linkbinding _ZN1n1gE, default
  .long 1

  .text
  .globl fb
fb:
  leaq _ZN1n1gE(%rip), %rax
  retq

#--- c.s
  .linkbindings
  .section .data,"dw",discard,_ZN1n1gE
  .globl _ZN1n1gE
_ZN1n1gE:
  .linkbinding _ZN1n1gE, default
  .long 1

  .text
  .globl fc
fc:
  callq *__imp_fa(%rip)
  movq __imp__ZN1n1gE(%rip), %rax
  retq

#--- main.s
  .text
  .globl main
main:
  callq *__imp_fa(%rip)
  callq *__imp_fb(%rip)
  retq

#--- main3.s
  .text
  .globl main
main:
  callq *__imp_fa(%rip)
  callq *__imp_fb(%rip)
  callq *__imp_fc(%rip)
  retq

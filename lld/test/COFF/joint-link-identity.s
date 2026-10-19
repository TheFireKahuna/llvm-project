# REQUIRES: x86
## The identities of a joint link's members. Every member that another imports
## from exports the identity the merge gives it, and every member imports the
## identity of each member it imports from. A member's identity covers what it
## and every member it imports from, directly or not, give the others beyond
## their import libraries, so that a file from another merge fails to load
## beside the files of this one wherever it is.
##
## a.dll and b.dll are true siblings, which app.exe loads, b.dll through c.dll.
## When a.dll starts to define w, which b.dll kept alone, a.dll keeps it for
## both: a.dll's identity changes, and so do b.dll's, which now imports w from
## a.dll, and c.dll's, whose own inputs did not change. Every importer from
## either merge then refuses the other's files.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium a.s -o a.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium a2.s -o a2.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium b.s -o b.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium c.s -o c.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium app.s -o app.obj

# RUN: mkdir r1 r2
# RUN: lld-link -import-slots -dll -noentry a.obj -out:a.dll \
# RUN:   -lldjointscan:a.jscan
# RUN: lld-link -import-slots -dll -noentry b.obj -out:b.dll \
# RUN:   -lldjointscan:b.jscan
# RUN: lld-link -import-slots -dll -noentry c.obj -out:c.dll \
# RUN:   -lldjointscan:c.jscan
# RUN: lld-link -import-slots -entry:main -subsystem:console app.obj \
# RUN:   -out:app.exe -lldjointscan:app.jscan
# RUN: lld-link -lldjointmerge -out:facts a.jscan b.jscan c.jscan app.jscan
# RUN: lld-link -import-slots -dll -noentry a.obj -out:r1/a.dll \
# RUN:   -lldjoint:facts/a.dll.jfacts
# RUN: lld-link -import-slots -dll -noentry b.obj -out:r1/b.dll \
# RUN:   -lldjoint:facts/b.dll.jfacts
# RUN: lld-link -import-slots -dll -noentry c.obj -out:r1/c.dll \
# RUN:   -lldjoint:facts/c.dll.jfacts
# RUN: lld-link -import-slots -entry:main -subsystem:console app.obj \
# RUN:   -out:r1/app.exe -lldjoint:facts/app.exe.jfacts

## c.dll imports b.dll's identity, and app.exe those of a.dll and c.dll; a
## member that keeps nothing of its own imports them too.
# RUN: llvm-readobj --coff-exports r1/a.dll r1/b.dll r1/c.dll > r1.txt
# RUN: llvm-readobj --coff-imports r1/c.dll r1/app.exe >> r1.txt
# RUN: FileCheck --check-prefix=R1 %s < r1.txt
# R1:      File: r1/a.dll
# R1:      Name: __llvm_private$[[A:[0-9a-f]{16}]]
# R1:      File: r1/b.dll
# R1:      Name: __llvm_private$[[B:[0-9a-f]{16}]]
# R1:      File: r1/c.dll
# R1:      Name: __llvm_private$[[C:[0-9a-f]{16}]]
# R1:      File: r1/c.dll
# R1:      Name: b.dll
# R1-NEXT: ImportLookupTableRVA:
# R1-NEXT: ImportAddressTableRVA:
# R1-NEXT: Symbol: __llvm_private$[[B]] (0)
# R1-NEXT: Symbol: (1)
# R1-NEXT: }
# R1:      File: r1/app.exe
# R1:      Name: a.dll
# R1-NEXT: ImportLookupTableRVA:
# R1-NEXT: ImportAddressTableRVA:
# R1-NEXT: Symbol: __llvm_private$[[A]] (0)
# R1-NEXT: Symbol: (1)
# R1-NEXT: }
# R1:      Name: c.dll
# R1-NEXT: ImportLookupTableRVA:
# R1-NEXT: ImportAddressTableRVA:
# R1-NEXT: Symbol: __llvm_private$[[C]] (0)
# R1-NEXT: Symbol: (1)
# R1-NEXT: }

## a.dll now defines w too and keeps it for b.dll, which imports it.
# RUN: lld-link -import-slots -dll -noentry a2.obj -out:a.dll \
# RUN:   -lldjointscan:a.jscan
# RUN: lld-link -lldjointmerge -out:facts2 a.jscan b.jscan c.jscan app.jscan
# RUN: lld-link -import-slots -dll -noentry a2.obj -out:r2/a.dll \
# RUN:   -lldjoint:facts2/a.dll.jfacts
# RUN: lld-link -import-slots -dll -noentry b.obj -out:r2/b.dll \
# RUN:   -lldjoint:facts2/b.dll.jfacts
# RUN: lld-link -import-slots -dll -noentry c.obj -out:r2/c.dll \
# RUN:   -lldjoint:facts2/c.dll.jfacts
# RUN: lld-link -import-slots -entry:main -subsystem:console app.obj \
# RUN:   -out:r2/app.exe -lldjoint:facts2/app.exe.jfacts
# RUN: llvm-readobj --coff-imports r2/b.dll | FileCheck --check-prefix=BIMP %s
# BIMP:      Name: a.dll
# BIMP-NEXT: ImportLookupTableRVA:
# BIMP-NEXT: ImportAddressTableRVA:
# BIMP-NEXT: Symbol: __llvm_private${{[0-9a-f]+}} (0)
# BIMP-NEXT: Symbol: (2)
# BIMP-NEXT: }

## Each of the three identities differs between the merges, and each importer
## of the second imports the new ones.
# RUN: llvm-readobj --coff-exports r1/a.dll | grep __llvm_private > r1/a.id
# RUN: llvm-readobj --coff-exports r2/a.dll | grep __llvm_private > r2/a.id
# RUN: llvm-readobj --coff-exports r1/b.dll | grep __llvm_private > r1/b.id
# RUN: llvm-readobj --coff-exports r2/b.dll | grep __llvm_private > r2/b.id
# RUN: llvm-readobj --coff-exports r1/c.dll | grep __llvm_private > r1/c.id
# RUN: llvm-readobj --coff-exports r2/c.dll | grep __llvm_private > r2/c.id
# RUN: not diff r1/a.id r2/a.id
# RUN: not diff r1/b.id r2/b.id
# RUN: not diff r1/c.id r2/c.id
# RUN: llvm-readobj --coff-exports r1/a.dll r1/b.dll r1/c.dll > ids.txt
# RUN: llvm-readobj --coff-exports r2/a.dll r2/b.dll r2/c.dll >> ids.txt
# RUN: llvm-readobj --coff-imports r2/c.dll r2/app.exe >> ids.txt
# RUN: FileCheck --check-prefix=R2 %s < ids.txt
# R2:      File: r1/a.dll
# R2:      Name: __llvm_private$[[A1:[0-9a-f]{16}]]
# R2:      File: r1/b.dll
# R2:      Name: __llvm_private$[[B1:[0-9a-f]{16}]]
# R2:      File: r1/c.dll
# R2:      Name: __llvm_private$[[C1:[0-9a-f]{16}]]
# R2:      File: r2/a.dll
# R2:      Name: __llvm_private$[[A2:[0-9a-f]{16}]]
# R2:      File: r2/b.dll
# R2:      Name: __llvm_private$[[B2:[0-9a-f]{16}]]
# R2:      File: r2/c.dll
# R2:      Name: __llvm_private$[[C2:[0-9a-f]{16}]]
# R2:      File: r2/c.dll
# R2:      Symbol: __llvm_private$[[B2]] (0)
# R2:      File: r2/app.exe
# R2:      Symbol: __llvm_private$[[A2]] (0)
# R2:      Symbol: __llvm_private$[[C2]] (0)

#--- a.s
  .text
  .globl af
af:
  retq

  .section .drectve,"yni"
  .ascii " -export:af"

#--- a2.s
  .linkbindings
  .section .data,"dw",discard,w
  .globl w
w:
  .linkbinding w, default
  .long 2

  .text
  .globl af
af:
  movq __imp_w(%rip), %rax
  retq

  .section .drectve,"yni"
  .ascii " -export:af"

#--- b.s
  .linkbindings
  .section .data,"dw",discard,w
  .globl w
w:
  .linkbinding w, default
  .long 1

  .text
  .globl bf
bf:
  movq __imp_w(%rip), %rax
  retq

  .section .drectve,"yni"
  .ascii " -export:bf"

#--- c.s
  .text
  .globl cf
cf:
  callq *__imp_bf(%rip)
  retq

  .section .drectve,"yni"
  .ascii " -export:cf"

#--- app.s
  .text
  .globl main
main:
  callq *__imp_af(%rip)
  callq *__imp_cf(%rip)
  retq

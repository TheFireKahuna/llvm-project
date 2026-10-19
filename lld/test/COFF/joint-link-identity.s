# REQUIRES: x86
## A member of a joint link that keeps its own copy of an observable vague
## entity imports the identity of each member it imports from, an executable
## too; a member that starts to define that entity keeps it for the others,
## which changes its identity, so that the old member fails to load beside it
## rather than keep a second copy.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium a.s -o a.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium b.s -o b.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium b2.s -o b2.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium app.s -o app.obj

# RUN: lld-link -import-slots -dll -noentry b.obj -out:b.dll \
# RUN:   -lldjointscan:b.jscan
# RUN: lld-link -import-slots -dll -noentry a.obj -out:a.dll \
# RUN:   -lldjointscan:a.jscan
# RUN: lld-link -import-slots -entry:main -subsystem:console app.obj \
# RUN:   -out:app.exe -lldjointscan:app.jscan
# RUN: lld-link -lldjointmerge -out:facts b.jscan a.jscan app.jscan
# RUN: lld-link -import-slots -dll -noentry b.obj -out:b.dll \
# RUN:   -lldjoint:facts/b.dll.jfacts
# RUN: lld-link -import-slots -dll -noentry a.obj -out:a.dll \
# RUN:   -lldjoint:facts/a.dll.jfacts
# RUN: lld-link -import-slots -entry:main -subsystem:console app.obj \
# RUN:   -out:app.exe -lldjoint:facts/app.exe.jfacts

# RUN: llvm-readobj --coff-exports b.dll > a.txt
# RUN: llvm-readobj --coff-imports a.dll app.exe >> a.txt
# RUN: FileCheck --check-prefix=A %s < a.txt
# A:      Name: __llvm_private$[[ID:[0-9a-f]{16}]]
# A:      File: a.dll
# A:      Name: b.dll
# A-NEXT: ImportLookupTableRVA:
# A-NEXT: ImportAddressTableRVA:
# A-NEXT: Symbol: __llvm_private$[[ID]] (0)
# A-NEXT: Symbol: bf (0)
# A-NEXT: }
# A:      File: app.exe
# A:      Name: b.dll
# A-NEXT: ImportLookupTableRVA:
# A-NEXT: ImportAddressTableRVA:
# A-NEXT: Symbol: __llvm_private$[[ID]] (0)
# A-NEXT: Symbol: bf (0)
# A-NEXT: }

## b.dll now defines w, and keeps it for the others under another identity;
## a.dll, which imports w from it and keeps nothing of its own, imports none.
# RUN: lld-link -import-slots -dll -noentry b2.obj -out:b.dll \
# RUN:   -lldjointscan:b.jscan
# RUN: lld-link -lldjointmerge -out:facts2 b.jscan a.jscan app.jscan
# RUN: lld-link -import-slots -dll -noentry b2.obj -out:b.dll \
# RUN:   -lldjoint:facts2/b.dll.jfacts
# RUN: lld-link -import-slots -dll -noentry a.obj -out:a.dll \
# RUN:   -lldjoint:facts2/a.dll.jfacts
# RUN: llvm-nm facts/a.dll.jfacts | grep 'T __imp___llvm_private' > id1.txt
# RUN: llvm-nm facts2/a.dll.jfacts | grep 'T __imp___llvm_private' > id2.txt
# RUN: FileCheck --check-prefix=FACT %s < id1.txt
# RUN: not diff id1.txt id2.txt
# FACT: __imp___llvm_private${{[0-9a-f]+}}$b.dll
# RUN: llvm-readobj --coff-imports a.dll | FileCheck --check-prefix=B %s
# B:      Name: b.dll
# B-NEXT: ImportLookupTableRVA:
# B-NEXT: ImportAddressTableRVA:
# B-NEXT: Symbol: bf (0)
# B-NEXT: Symbol: w (0)
# B-NEXT: }

#--- a.s
  .linkbindings
  .section .data,"dw",discard,w
  .globl w
w:
  .linkbinding w, default
  .long 1

  .text
  .globl af
af:
  movq __imp_w(%rip), %rax
  callq *__imp_bf(%rip)
  retq

  .section .drectve,"yni"
  .ascii " -export:af"

#--- b.s
  .text
  .globl bf
bf:
  retq

  .section .drectve,"yni"
  .ascii " -export:bf"

#--- b2.s
  .linkbindings
  .section .data,"dw",discard,w
  .globl w
w:
  .linkbinding w, default
  .long 2

  .text
  .globl bf
bf:
  movq __imp_w(%rip), %rax
  retq

  .section .drectve,"yni"
  .ascii " -export:bf"

#--- app.s
  .linkbindings
  .section .data,"dw",discard,u
  .globl u
u:
  .linkbinding u, default
  .long 1

  .text
  .globl main
main:
  movq __imp_u(%rip), %rax
  callq *__imp_bf(%rip)
  retq

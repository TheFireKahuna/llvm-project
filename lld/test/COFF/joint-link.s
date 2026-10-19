# REQUIRES: x86
## A joint link. Each member's link line runs with /lldjointscan, which writes
## the member's summary and no image, and its import library where none is
## there yet; /lldjointmerge reads the summaries and writes each member a fact
## file, an empty one too; and each member's final link reads its own with
## /lldjoint. Members reference what other members export with no import
## library, circularly, and of the observable vague entities that several
## members keep, true siblings among them, one member keeps each for the
## others, which import it: a member the others already import from, else the
## first declared. An executable keeps none for the others, and several may be
## members.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium a.s -o a.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium b.s -o b.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium app.s -o app.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium tool.s -o tool.obj

# RUN: lld-link -import-slots -dll -noentry a.obj -out:a.dll \
# RUN:   -lldjointscan:a.jscan
# RUN: lld-link -import-slots -dll -noentry b.obj -out:b.dll \
# RUN:   -lldjointscan:b.jscan
# RUN: lld-link -import-slots -entry:main -subsystem:console app.obj a.lib \
# RUN:   b.lib -out:app.exe -lldjointscan:app.jscan
# RUN: lld-link -import-slots -entry:main -subsystem:console tool.obj \
# RUN:   -out:tool.exe -lldjointscan:tool.jscan
# RUN: not ls a.dll app.exe
# RUN: llvm-nm a.lib | FileCheck --check-prefix=SCANLIB %s
# SCANLIB: __imp_af
# SCANLIB: __imp_v

# RUN: lld-link -lldjointmerge -out:facts a.jscan b.jscan app.jscan tool.jscan
# RUN: ls facts | FileCheck --check-prefix=FACTS %s
# FACTS:      a.dll.jfacts
# FACTS-NEXT: app.exe.jfacts
# FACTS-NEXT: b.dll.jfacts
# FACTS-NEXT: tool.exe.jfacts
# RUN: cp -r facts facts1
# RUN: lld-link -lldjointmerge -out:facts a.jscan b.jscan app.jscan tool.jscan
# RUN: diff -r facts facts1

# RUN: lld-link -import-slots -dll -noentry a.obj -out:a.dll \
# RUN:   -lldjoint:facts/a.dll.jfacts
# RUN: lld-link -import-slots -dll -noentry b.obj -out:b.dll \
# RUN:   -lldjoint:facts/b.dll.jfacts
# RUN: lld-link -import-slots -entry:main -subsystem:console app.obj a.lib \
# RUN:   b.lib -out:app.exe -lldjoint:facts/app.exe.jfacts
# RUN: lld-link -import-slots -entry:main -subsystem:console tool.obj \
# RUN:   -out:tool.exe -lldjoint:facts/tool.exe.jfacts

## a.dll, which b.dll imports from, keeps v for the others and offers it.
# RUN: llvm-readobj --coff-imports --coff-exports a.dll | \
# RUN:   FileCheck --check-prefix=A %s
# A:      Name: b.dll
# A-NEXT: ImportLookupTableRVA:
# A-NEXT: ImportAddressTableRVA:
# A-NEXT: Symbol: bf (0)
# A-NEXT: }
# A:      Name: af
# A:      Name: v

# RUN: llvm-readobj --coff-imports --coff-exports b.dll | \
# RUN:   FileCheck --check-prefix=B %s
# B:      Name: a.dll
# B-NEXT: ImportLookupTableRVA:
# B-NEXT: ImportAddressTableRVA:
# B-NEXT: Symbol: af (0)
# B-NEXT: Symbol: v (0)
# B-NEXT: }
# B:      Name: bf
# B:      Name: v
# B-NEXT: ForwardedTo: a.v

# RUN: llvm-readobj --coff-imports app.exe | FileCheck --check-prefix=APP %s
# APP:      Name: a.dll
# APP-NEXT: ImportLookupTableRVA:
# APP-NEXT: ImportAddressTableRVA:
# APP-NEXT: Symbol: v (0)
# APP-NEXT: }
# APP:      Name: b.dll
# APP-NEXT: ImportLookupTableRVA:
# APP-NEXT: ImportAddressTableRVA:
# APP-NEXT: Symbol: bf (0)
# APP-NEXT: }

# RUN: llvm-readobj --coff-imports tool.exe | FileCheck --check-prefix=TOOL %s
# TOOL:      Name: a.dll
# TOOL-NEXT: ImportLookupTableRVA:
# TOOL-NEXT: ImportAddressTableRVA:
# TOOL-NEXT: Symbol: af (0)
# TOOL-NEXT: Symbol: v (0)
# TOOL-NEXT: }

#--- a.s
  .linkbindings
  .section .data,"dw",discard,v
  .globl v
v:
  .linkbinding v, default
  .long 1

  .text
  .globl af
af:
  movq __imp_v(%rip), %rax
  movl (%rax), %eax
  callq *__imp_bf(%rip)
  retq

  .section .drectve,"yni"
  .ascii " -export:af"

#--- b.s
  .linkbindings
  .section .data,"dw",discard,v
  .globl v
v:
  .linkbinding v, default
  .long 1

  .text
  .globl bf
bf:
  movq __imp_v(%rip), %rax
  movl (%rax), %eax
  callq *__imp_af(%rip)
  retq

  .section .drectve,"yni"
  .ascii " -export:bf"

#--- app.s
  .linkbindings
  .section .data,"dw",discard,v
  .globl v
v:
  .linkbinding v, default
  .long 1

  .text
  .globl main
main:
  movq __imp_v(%rip), %rax
  movl (%rax), %eax
  callq *__imp_bf(%rip)
  retq

#--- tool.s
  .linkbindings
  .section .data,"dw",discard,v
  .globl v
v:
  .linkbinding v, default
  .long 1

  .text
  .globl main
main:
  movq __imp_v(%rip), %rax
  movl (%rax), %eax
  callq *__imp_af(%rip)
  retq

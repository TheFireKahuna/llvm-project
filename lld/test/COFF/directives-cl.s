# REQUIRES: x86

## cl.exe's section directives act, as in link.exe, on every input section of
## the name they give, from a directive in any object or bitcode file.
## /icfsection lets ICF fold such read-only data; /coffsecttopdbstrm moves such
## a section into a named PDB stream, with or without /debug, the files'
## sections in reverse order; /includecoffsection keeps it under /opt:ref.
## /wholearchive loads every member of a library, and bare, of every library
## the command line names.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc a.s -o a.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc dir.s -o dir.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc b.s -o b.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc m1.s -o m1.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc m2.s -o m2.obj
# RUN: llvm-as bc.ll -o bc.obj
# RUN: llvm-lib -out:m1.lib m1.obj
# RUN: llvm-lib -out:m2.lib m2.obj
# RUN: lld-link -entry:start -subsystem:console -opt:ref,icf -debug -map \
# RUN:   -out:out.exe a.obj dir.obj b.obj bc.obj m2.lib -verbose 2>&1 \
# RUN:   | FileCheck --check-prefix=ICF %s
# RUN: llvm-readobj --sections out.exe | FileCheck %s
# RUN: llvm-pdbutil export -stream=mystrm -out=- out.pdb \
# RUN:   | FileCheck --check-prefix=PDB %s
# RUN: llvm-pdbutil export -stream=bcstrm -out=- out.pdb \
# RUN:   | FileCheck --check-prefix=PDB-BC %s
# RUN: FileCheck --check-prefix=MAP %s < out.map
# RUN: lld-link -entry:start -subsystem:console -opt:ref -out:nodebug.exe \
# RUN:   a.obj dir.obj b.obj bc.obj m2.lib
# RUN: llvm-readobj --sections nodebug.exe | FileCheck %s

# ICF:        Selected a1
# ICF-NEXT:     Removed a2
# ICF-NEXT:     Removed b1
# ICF-NEXT:     Removed b2

# CHECK-NOT:  Name: .pdbdata
# CHECK-NOT:  Name: .bcdata
# CHECK:      Name: .keep
# CHECK-NOT:  Name: .pdbdata
# CHECK-NOT:  Name: .bcdata

# PDB: {{^}}CCCCBBBBAAAA{{$}}
# PDB-BC: {{^}}EEEE{{$}}

# MAP-DAG: {{ }}m1sym{{ }}
# MAP-DAG: {{ }}m2sym{{ }}
# MAP-DAG: {{ }}k1{{ }}

## The arguments of /coffsecttopdbstrm.
# RUN: env LLD_IN_TEST=1 not lld-link -entry:start -out:bad.exe a.obj \
# RUN:   -coffsecttopdbstrm:.pdbdata -coffsecttopdbstrm:.pdbdata= 2>&1 \
# RUN:   | FileCheck --check-prefix=BAD %s
# BAD: error: /coffsecttopdbstrm: invalid argument: .pdbdata{{$}}
# BAD: error: /coffsecttopdbstrm: invalid argument: .pdbdata={{$}}

#--- a.s
  .text
  .globl start
start:
  leaq a1(%rip), %rax
  leaq a2(%rip), %rax
  leaq b1(%rip), %rax
  leaq b2(%rip), %rax
  retq
  .section .rdata,"dr",discard,a1
  .globl a1
a1:
  .quad 1
  .section .rdata,"dr",discard,a2
  .globl a2
a2:
  .quad 1
  .section .pdbdata,"dr"
  .ascii "AAAA"
  .section .keep,"dr",discard,k1
  .globl k1
k1:
  .quad 1

#--- dir.s
  .section .drectve,"yn"
  .ascii " /icfsection:.rdata /coffsecttopdbstrm:.pdbdata=mystrm"
  .ascii " /includecoffsection:.keep /wholearchive:m1.lib /wholearchive"
  .section .pdbdata,"dr"
  .ascii "BBBB"

#--- b.s
  .section .rdata,"dr",discard,b1
  .globl b1
b1:
  .quad 1
  .section .rdata,"dr",discard,b2
  .globl b2
b2:
  .quad 1
  .section .pdbdata,"dr"
  .ascii "CCCC"

#--- bc.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

@bcdata = constant [4 x i8] c"EEEE", section ".bcdata"
@llvm.used = appending global [1 x ptr] [ptr @bcdata], section "llvm.metadata"

!llvm.linker.options = !{!0}
!0 = !{!"/coffsecttopdbstrm:.bcdata=bcstrm"}

#--- m1.s
  .data
  .globl m1sym
m1sym:
  .long 1

#--- m2.s
  .data
  .globl m2sym
m2sym:
  .long 2

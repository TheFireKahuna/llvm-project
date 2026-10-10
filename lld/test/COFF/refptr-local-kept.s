# REQUIRES: x86

## A .refptr.X pointer bound to a local import pointer stays the pointer,
## where its section is, whenever the image needs one: when data holds its
## address, or when another name in its section is referenced. Every name then
## gives that one address, and the linker makes no pointer of its own. Without
## /opt:ref an external name in the section counts as referenced. A pointer
## that only rewritten instructions read is left out, whether or not sections
## are collected.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium defs.s -o defs.obj

# RUN: lld-link -entry:main -subsystem:console -out:a.exe main.obj defs.obj \
# RUN:   -map:a.map
# RUN: llvm-objdump -d a.exe | FileCheck --check-prefix=CODE %s
# RUN: llvm-objdump -s -j .rdata -j .data a.exe | FileCheck %s
# RUN: FileCheck --check-prefix=MAP %s < a.map

# RUN: lld-link -entry:main -subsystem:console -out:b.exe main.obj defs.obj \
# RUN:   -opt:noref -map:b.map
# RUN: llvm-objdump -d b.exe | FileCheck --check-prefix=CODE %s
# RUN: llvm-objdump -s -j .rdata -j .data b.exe | \
# RUN:   FileCheck --check-prefix=NOREF %s
# RUN: FileCheck --check-prefixes=MAP,NOREFMAP %s < b.map

# CODE:      48 8d 05 {{.*}} leaq {{.*}}(%rip), %rax # 0x140003010
# CODE-NEXT: 48 8d 0d {{.*}} leaq {{.*}}(%rip), %rcx # 0x140003014
# CODE-NEXT: 48 8d 15 {{.*}} leaq {{.*}}(%rip), %rdx # 0x140003018
# CODE-NEXT: 48 8d 35 {{.*}} leaq {{.*}}(%rip), %rsi # 0x14000301c
# CODE-NEXT: 48 8d 3d {{.*}} leaq {{.*}}(%rip), %rdi # 0x140003020

## The pointers to a and b, and the words of data that hold their addresses.
# CHECK:      Contents of section .rdata:
# CHECK-NEXT: 140002000 10300040 01000000 14300040 01000000 {{.*$}}
# CHECK-NEXT: Contents of section .data:
# CHECK-NEXT: 140003000 00200040 01000000 08200040 01000000

## Without /opt:ref, the pointer to d as well.
# NOREF:      Contents of section .rdata:
# NOREF-NEXT: 140002000 10300040 01000000 14300040 01000000
# NOREF-NEXT: 140002010 1c300040 01000000 {{.*$}}
# NOREF-NEXT: Contents of section .data:
# NOREF-NEXT: 140003000 00200040 01000000 08200040 01000000

# MAP-DAG:   .refptr.a 0000000140002000
# MAP-DAG:   .refptr.b 0000000140002008
# MAP-DAG:   balias 0000000140002008
# NOREFMAP-DAG: .refptr.d 0000000140002010
# NOREFMAP-DAG: dalias 0000000140002010

#--- main.s
  .text
  .globl main
main:
  movq .refptr.a(%rip), %rax
  movq .refptr.b(%rip), %rcx
  movq .refptr.c(%rip), %rdx
  movq .refptr.d(%rip), %rsi
  movq .refptr.e(%rip), %rdi
  retq

  .data
  .quad .refptr.a
  .quad balias

  .section .rdata$.refptr.a,"dr",discard,.refptr.a
  .globl .refptr.a
.refptr.a:
  .quad a

## A local name, referenced from data.
  .section .rdata$.refptr.b,"dr",discard,.refptr.b
  .globl .refptr.b
.refptr.b:
balias:
  .quad b

  .section .rdata$.refptr.c,"dr",discard,.refptr.c
  .globl .refptr.c
.refptr.c:
  .quad c

## An external name, which any object may refer to.
  .section .rdata$.refptr.d,"dr",discard,.refptr.d
  .globl .refptr.d
  .globl dalias
.refptr.d:
dalias:
  .quad d

## A local name that nothing refers to.
  .section .rdata$.refptr.e,"dr",discard,.refptr.e
  .globl .refptr.e
.refptr.e:
elabel:
  .quad e

#--- defs.s
  .data
  .globl a, b, c, d, e
a:
  .long 1
b:
  .long 2
c:
  .long 3
d:
  .long 4
e:
  .long 5

# REQUIRES: x86

## A .refptr.X pointer bound to a local import pointer stays the pointer,
## where its section is, whenever the image needs one: when data holds its
## address, when another name in its section is referenced, and when sections
## are not collected. Every name then gives that one address, and the linker
## makes no pointer of its own. A pointer that only rewritten instructions
## read is left out once sections are collected.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium defs.s -o defs.obj

# RUN: lld-link -entry:main -subsystem:console -out:a.exe main.obj defs.obj \
# RUN:   -map:a.map
# RUN: llvm-objdump -d a.exe | FileCheck --check-prefix=CODE %s
# RUN: llvm-objdump -s -j .rdata -j .data a.exe | FileCheck %s
# RUN: FileCheck --check-prefix=MAP %s < a.map

# CODE:      48 8d 05 {{.*}} leaq {{.*}}(%rip), %rax # 0x140003010
# CODE-NEXT: 48 8d 0d {{.*}} leaq {{.*}}(%rip), %rcx # 0x140003014
# CODE-NEXT: 48 8d 15 {{.*}} leaq {{.*}}(%rip), %rdx # 0x140003018

## The pointers to a and b, and the words of data that hold their addresses.
# CHECK:      Contents of section .rdata:
# CHECK-NEXT: 140002000 10300040 01000000 14300040 01000000
# CHECK-NEXT: Contents of section .data:
# CHECK-NEXT: 140003000 00200040 01000000 08200040 01000000

# MAP: .refptr.a 0000000140002000
# MAP: .refptr.b 0000000140002008
# MAP: balias 0000000140002008

# RUN: lld-link -entry:main -subsystem:console -out:b.exe main.obj defs.obj \
# RUN:   -opt:noref
# RUN: llvm-objdump -d b.exe | FileCheck --check-prefix=CODE %s
# RUN: llvm-objdump -s -j .rdata b.exe | FileCheck --check-prefix=NOREF %s

# NOREF:      Contents of section .rdata:
# NOREF-NEXT: 140002000 10300040 01000000 14300040 01000000
# NOREF-NEXT: 140002010 18300040 01000000

#--- main.s
  .text
  .globl main
main:
  movq .refptr.a(%rip), %rax
  movq .refptr.b(%rip), %rcx
  movq .refptr.c(%rip), %rdx
  retq

  .data
  .quad .refptr.a
  .quad balias

  .section .rdata$.refptr.a,"dr",discard,.refptr.a
  .globl .refptr.a
.refptr.a:
  .quad a

  .section .rdata$.refptr.b,"dr",discard,.refptr.b
  .globl .refptr.b
.refptr.b:
balias:
  .quad b

  .section .rdata$.refptr.c,"dr",discard,.refptr.c
  .globl .refptr.c
.refptr.c:
  .quad c

#--- defs.s
  .data
  .globl a, b, c
a:
  .long 1
b:
  .long 2
c:
  .long 3

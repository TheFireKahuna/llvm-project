# REQUIRES: x86

## A .refptr.X pointer, which a compiler reads for an extern_weak X, is a
## local import pointer to X once X is defined in the image, or to zero once a
## weak X is absent: a described load of it becomes the lea of X or a move of
## zero, and a call through it a direct call, or a call of zero through R11. A
## pointer that only such references read is left out of the image; one that
## holds zero has no base relocation. A jump through a pointer that holds zero
## still reads it. A reference through a pointer is not reported as an
## imported local. -import-slots changes none of it.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium defs.s -o defs.obj

# RUN: lld-link -entry:main -subsystem:console -out:a.exe main.obj defs.obj \
# RUN:   2>&1 | count 0
# RUN: llvm-objdump -d a.exe | FileCheck %s
# RUN: llvm-readobj --coff-basereloc a.exe | FileCheck --check-prefix=RELOC %s
# RUN: lld-link -import-slots -entry:main -subsystem:console -out:b.exe \
# RUN:   main.obj defs.obj 2>&1 | count 0
# RUN: llvm-objdump -d b.exe | FileCheck %s

# CHECK:      48 8d 05 {{.*}} leaq {{.*}}(%rip), %rax # 0x[[#%x,F:]]
# CHECK-NEXT: 67 e8 {{.*}} addr32 callq 0x[[#F]]
# CHECK-NEXT: 48 8d 0d {{.*}} leaq {{.*}}(%rip), %rcx # 0x[[#%x,V:]]
# CHECK-NEXT: 49 c7 c1 00 00 00 00 movq $0x0, %r9
# CHECK-NEXT: 48 c7 c2 00 00 00 00 movq $0x0, %rdx
# CHECK-NEXT: 45 31 db xorl %r11d, %r11d
# CHECK-NEXT: 41 ff d3 callq *%r11
# CHECK-NEXT: 48 83 3d {{.*}} cmpq $0x0, {{.*}}(%rip)
# CHECK-NEXT: c3 retq
# CHECK-NEXT: ff 25 {{.*}} jmpq *{{.*}}(%rip)
# CHECK:      [[#%x,F]]: c3 retq

## The pointer to w, which the compare reads; the pointer that holds zero for
## the jump through absent3 has none.
# RELOC-COUNT-1: Type: DIR64
# RELOC-NOT:     Type: DIR64

#--- main.s
  .text
  .globl main
main:
  movq .refptr.f(%rip), %rax
  callq *.refptr.f(%rip)
  movq .refptr.v(%rip), %rcx
  movq .refptr.absent(%rip), %r9
  movq .refptr.absent2(%rip), %rdx
  callq *.refptr.absent(%rip)
  cmpq $0, .refptr.w(%rip)
  retq
  jmpq *.refptr.absent3(%rip)

  .weak f
  .weak v
  .weak w
  .weak absent
  .weak absent2
  .weak absent3

.macro refptr sym
  .section .rdata$.refptr.\sym,"dr",discard,.refptr.\sym
  .globl .refptr.\sym
.refptr.\sym:
  .quad \sym
.endm
  refptr f
  refptr v
  refptr w
  refptr absent
  refptr absent2
  refptr absent3

#--- defs.s
  .text
  .globl f
f:
  retq

  .data
  .globl v
v:
  .long 1
  .globl w
w:
  .long 2

# REQUIRES: x86
## A function that ends in a jump to a function whose code the image places
## right after it leaves the jump out, so that it runs into its target and
## both begin at the same address, where the image takes no address of the
## target. A target whose address is taken, one that needs more alignment
## than the jump's function has, and a link that builds no guard tables, which
## collect the addresses taken of functions, keep the jump.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium taken.s -o taken.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium aligned.s -o aligned.obj
# RUN: lld-link -guard:cf -entry:main -subsystem:console main.obj -out:main.exe \
# RUN:   -verbose 2>&1 | FileCheck --check-prefix=LOG %s
# RUN: llvm-objdump -d main.exe | FileCheck %s
# RUN: lld-link -guard:cf -entry:main -subsystem:console taken.obj \
# RUN:   -out:taken.exe
# RUN: llvm-objdump -d taken.exe | FileCheck --check-prefix=KEEP %s
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium main.s -o plain.obj
# RUN: lld-link -entry:main -subsystem:console plain.obj -out:plain.exe
# RUN: llvm-objdump -d plain.exe | FileCheck --check-prefix=KEEP %s
# RUN: lld-link -guard:cf -entry:main -subsystem:console aligned.obj \
# RUN:   -out:aligned.exe
# RUN: llvm-objdump -d aligned.exe | FileCheck --check-prefix=KEEP %s

# LOG: Left out 1 of 1 jumps that end their functions before their targets

# CHECK:      <.text>:
# CHECK-NEXT: callq 0x[[#%x,FWD:]]
# CHECK-NEXT: retq
# CHECK:      [[#%x,FWD]]: b8 01 00 00 00 movl $0x1, %eax
# CHECK-NEXT: c3 retq

# KEEP: jmp

#--- main.s
  .text
  .globl main
main:
  callq fwd
  retq

  .section .text$fwd,"xr"
  .globl fwd
  .p2align 4
fwd:
  .linktailjump
  jmp impl

  .section .text$impl,"xr"
  .def impl
  .scl 3
  .type 32
  .endef
  .p2align 4
impl:
  movl $1, %eax
  retq

#--- taken.s
  .text
  .globl main
main:
  leaq impl(%rip), %rax
  callq fwd
  retq

  .section .text$fwd,"xr"
  .globl fwd
  .p2align 4
fwd:
  .linktailjump
  jmp impl

  .section .text$impl,"xr"
  .def impl
  .scl 3
  .type 32
  .endef
  .p2align 4
impl:
  movl $1, %eax
  retq


#--- aligned.s
  .text
  .globl main
main:
  callq fwd
  retq

  .section .text$fwd,"xr"
  .globl fwd
  .p2align 4
fwd:
  .linktailjump
  jmp impl

  .section .text$impl,"xr"
  .def impl
  .scl 3
  .type 32
  .endef
  .p2align 5
impl:
  movl $1, %eax
  retq

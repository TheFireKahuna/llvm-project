# REQUIRES: x86

## An object that does not describe its sites reads a .refptr.X pointer bound
## to a local import pointer only from its live sections. Once its reading
## function is collected, the pointer goes, though the object still lists
## it; while the function is live, the pointer stays, where it is.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium call.s -o call.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-gnu foreign.s \
# RUN:   -o foreign.obj

# RUN: lld-link -entry:main -subsystem:console -out:a.exe main.obj \
# RUN:   foreign.obj
# RUN: llvm-readobj --coff-basereloc a.exe | FileCheck --check-prefix=NONE %s
# RUN: llvm-objdump -d a.exe | FileCheck --check-prefix=LEA %s

# NONE-NOT: Type: DIR64
# LEA:      48 8d 05 {{.*}} leaq {{.*}}(%rip), %rax

# RUN: lld-link -entry:main -subsystem:console -out:b.exe main.obj call.obj \
# RUN:   foreign.obj
# RUN: llvm-readobj --coff-basereloc b.exe | FileCheck --check-prefix=ONE %s
# RUN: llvm-objdump -d b.exe | FileCheck --check-prefix=READ %s

# ONE-COUNT-1: Type: DIR64
# ONE-NOT:     Type: DIR64
# READ:      48 8d 05 {{.*}} leaq {{.*}}(%rip), %rax
# READ:      48 8b 05 {{.*}} movq {{.*}}(%rip), %rax # 0x140002000

#--- main.s
  .text
  .globl main
main:
  movq .refptr.a(%rip), %rax
  retq

  .data
  .globl a
a:
  .long 1

#--- call.s
  .text
  .globl call
call:
  jmp reader

  .section .drectve,"yni"
  .ascii " -include:call"

#--- foreign.s
  .section .text$reader,"xr",discard,reader
  .globl reader
reader:
  movq .refptr.a(%rip), %rax
  retq

  .section .rdata$.refptr.a,"dr",discard,.refptr.a
  .globl .refptr.a
.refptr.a:
  .quad a

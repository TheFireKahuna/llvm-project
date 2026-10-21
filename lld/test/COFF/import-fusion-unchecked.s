# REQUIRES: x86

## A fused call or access is rewritten only in an object whose assembler
## checked its facts against the instructions, which the object's link-only
## records say. Elsewhere it stays as compiled, through the register its load
## fills, and the load becomes an lea of the function or data in the image.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium main.s -o main.obj
# RUN: obj2yaml main.obj | sed -e 's/4C4C5243010F/4C4C52430107/' | \
# RUN:   yaml2obj -o unchecked.obj
# RUN: lld-link -entry:main -subsystem:console -out:a.exe main.obj
# RUN: llvm-objdump -d --no-show-raw-insn a.exe | FileCheck %s
# RUN: lld-link -entry:main -subsystem:console -out:b.exe unchecked.obj
# RUN: llvm-objdump -d --no-show-raw-insn b.exe | \
# RUN:   FileCheck %s --check-prefix=UNCHECKED

# CHECK:      <.text>:
# CHECK-NEXT: callq 0x[[#%x,LOCAL:]]
# CHECK-NEXT: movl {{.*}}(%rip), %eax
# CHECK-NEXT: retq
# CHECK-NEXT: [[#LOCAL]]: retq

# UNCHECKED:      <.text>:
# UNCHECKED-NEXT: leaq {{.*}}(%rip), %rax
# UNCHECKED-NEXT: callq *%rax
# UNCHECKED-NEXT: leaq {{.*}}(%rip), %rcx
# UNCHECKED-NEXT: movl (%rcx), %eax
# UNCHECKED-NEXT: retq

#--- main.s
  .text
  .globl main
main:
.La:
  .linkfusedread
  movq __imp_local(%rip), %rax
  .linkfusedcall .La, %rax
  cs; cs; cs; cs
  callq *%rax
.Lb:
  .linkfusedread
  movq __imp_v(%rip), %rcx
  .linkfusedaccess .Lb, %rcx
  movl (%rcx), %eax
  retq

  .globl local
local:
  retq

  .data
  .globl v
v:
  .long 1

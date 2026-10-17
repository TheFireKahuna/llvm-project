# REQUIRES: x86
## A jump that the assembler gave a 32-bit displacement takes 8 bits again
## where the code it crosses gets shorter enough, here calls through the
## import pointer of a function in the image that become direct ones; one
## that still crosses too much keeps 32 bits.

# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium %s -o %t.obj
# RUN: lld-link -entry:main -subsystem:console %t.obj -out:%t.exe -verbose \
# RUN:   2>&1 | FileCheck --check-prefix=LOG %s
# RUN: llvm-objdump -d %t.exe | FileCheck %s

# LOG: Relaxed 1 of 1 covered sections, 9 bytes smaller

# CHECK:      <.text>:
# CHECK-NEXT: 85 c9 testl %ecx, %ecx
# CHECK-NEXT: 74 7d je 0x[[#%x,NEAR:]]
# CHECK:      [[#NEAR]]: 85 d2 testl %edx, %edx
# CHECK-NEXT: 0f 84 82 00 00 00 je

  .text
  .globl main
main:
  testl %ecx, %ecx
  je .Lnear
  callq *__imp_local(%rip)
  callq *__imp_local(%rip)
  callq *__imp_local(%rip)
  callq *__imp_local(%rip)
  .fill 105, 1, 0x90
.Lnear:
  testl %edx, %edx
  je .Lfar
  callq *__imp_local(%rip)
  .fill 125, 1, 0x90
.Lfar:
  retq

  .globl local
local:
  retq

# REQUIRES: x86

## A jump out of a section with spans is two bytes where the chunks' order puts
## its target in reach, forward or back, through a bound pointer too; a jump
## past more than 8 bits of chunk keeps its width. No size depends on an address.

# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium %s -o %t.obj
# RUN: lld-link -dll -noentry -export:a -export:d %t.obj -out:%t.dll
# RUN: llvm-objdump -d %t.dll | FileCheck %s

# CHECK:      <a>:
# CHECK-NEXT: 180001000: 75 0e          jne 0x180001010
# CHECK-NEXT: 180001002: eb 01          jmp 0x180001005
# CHECK-NEXT: 180001004: c3             retq
# CHECK-NEXT: 180001005: 90             nop
# CHECK-NEXT: 180001006: eb f8          jmp 0x180001000
# CHECK-NEXT: 180001008: e9 d2 00 00 00 jmp 0x1800010df
# CHECK-NEXT: 18000100d: eb 01          jmp 0x180001010
# CHECK-NEXT: 18000100f: c3             retq
# CHECK-NEXT: 180001010: 90             nop
# CHECK-NEXT: 180001011: e9 c9 00 00 00 jmp 0x1800010df
# CHECK-NEXT: 180001016: c3             retq

  .section .text,"xr",discard,a
  .globl a
a:
  jne c
  jmp b
  retq

  .section .text,"xr",discard,b
  .globl b
b:
  nop
  jmp a
  jmp far
  jmpq *__imp_c(%rip)
  retq

  .section .text,"xr",discard,c
  .globl c
c:
  nop
  jmpq *__imp_far(%rip)
  retq

  .section .text,"xr",discard,d
  .globl d
d:
  .space 200, 0xcc

  .section .text,"xr",discard,far
  .globl far
far:
  retq

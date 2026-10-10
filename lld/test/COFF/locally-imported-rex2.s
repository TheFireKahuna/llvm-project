# REQUIRES: x86

## A described pointer load with a REX2 prefix, through the local import
## pointer of a symbol in the image, becomes the lea of the symbol; through a
## pointer to an absent weak symbol's zero, `mov r/m64, imm32` into the same
## register, whose high bits move from R4 and R3 to B4 and B3 of the prefix.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium -mattr=+egpr \
# RUN:   main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium defs.s -o defs.obj
# RUN: lld-link -entry:main -subsystem:console -out:a.exe main.obj defs.obj \
# RUN:   2>&1 | count 0
# RUN: llvm-objdump -d a.exe | FileCheck %s

# CHECK:      d5 48 8d 05 {{.*}} leaq {{.*}}(%rip), %r16 # 0x[[#%x,F:]]
# CHECK-NEXT: d5 18 c7 c4 00 00 00 00 movq $0x0, %r20
# CHECK-NEXT: d5 19 c7 c1 00 00 00 00 movq $0x0, %r25
# CHECK-NEXT: 48 c7 c1 00 00 00 00 movq $0x0, %rcx
# CHECK-NEXT: 49 c7 c1 00 00 00 00 movq $0x0, %r9
# CHECK-NEXT: c3 retq
# CHECK:      [[#%x,F]]: c3 retq

#--- main.s
  .text
  .globl main
main:
  movq __imp_f(%rip), %r16
  movq __imp_absent(%rip), %r20
  movq __imp_absent(%rip), %r25
  movq __imp_absent(%rip), %rcx
  movq __imp_absent(%rip), %r9
  retq

  .weak absent

#--- defs.s
  .text
  .globl f
  .p2align 4
f:
  retq

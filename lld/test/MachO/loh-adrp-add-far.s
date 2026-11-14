# REQUIRES: aarch64

## A target just under 4 GiB before an adrp+add pair is within adrp's range but
## far outside adr's. Its distance must not be taken for its low 32 bits.

# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %s -o %t.o
# RUN: %lld -arch arm64 -dylib %t.o -o %t
# RUN: llvm-objdump -d --macho --section=__FAR,__text %t | FileCheck %s

# CHECK-LABEL: _far:
# CHECK-NEXT: adrp x0
# CHECK-NEXT: add x0, x0

.text
.globl _target
_target:
  ret

.section __FAR,__text,regular,pure_instructions
.globl _far
.p2align 2
_far:
L1:
  adrp x0, _target@PAGE
L2:
  add  x0, x0, _target@PAGEOFF
  ret

.loh AdrpAdd L1, L2

## Last, so that the object's sections are in address order.
.zerofill __DATA,__bss,_gap,0xfff80000,0

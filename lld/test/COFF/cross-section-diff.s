# REQUIRES: x86
# RUN: llvm-mc -triple x86_64-windows-msvc -filetype=obj %s -o %t.obj
# RUN: lld-link -entry:main -out:%t.exe %t.obj
# RUN: llvm-objdump -s -j .rdata %t.exe | FileCheck %s

## The assembler writes a 32-bit difference across sections as SSPAN32 and
## PAIR, which the linker resolves to A - B in the image.

# CHECK: 140002000 0b000000 0c000000

  .text
  .def main; .scl 2; .type 32; .endef
  .globl main
main:
  ret
.Lb:
  .fill 11, 1, 0xcc
  .def f; .scl 3; .type 32; .endef
f:
  ret
  ret

  .section .rdata,"dr"
  .long f - .Lb
  .long f - .Lb + 1

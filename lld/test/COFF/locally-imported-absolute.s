# REQUIRES: x86

## The local import pointer of an absolute symbol holds the symbol's value,
## which does not move with the image, so it has no base relocation.

# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %s -o %t.obj
# RUN: lld-link -entry:main -subsystem:console -out:%t.exe %t.obj 2>&1 \
# RUN:   | FileCheck --check-prefix=WARN %s
# RUN: llvm-readobj --coff-basereloc %t.exe | FileCheck --check-prefix=RELOC %s
# RUN: llvm-objdump -s -j .rdata %t.exe | FileCheck --check-prefix=DATA %s

# WARN: locally defined symbol imported: abs

# RELOC-NOT: Type: DIR64

# DATA: 34120000 00000000

  .text
  .globl main
main:
  movq __imp_abs(%rip), %rax
  retq

  .globl abs
  .set abs, 0x1234

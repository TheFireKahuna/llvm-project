# REQUIRES: x86

## A KCFI prefix that carries a second type word before the marker is read
## from 16 bytes before the end of the marker pattern, so a check reads
## nothing before a target in the page's first PowerOf2Ceil(prefix + 16)
## bytes. The linker keeps each such entry out of that
## many bytes: here 4 bytes of patchable prefix keep h's entry out of the
## first 32, where a prefix without the second word would need only 16.

# RUN: llvm-mc -triple x86_64-windows-msvc %s -filetype=obj -o %t.obj
# RUN: llvm-mc -triple x86_64-windows-msvc %s -filetype=obj --defsym NOREC=1 \
# RUN:   -o %t.norec.obj
# RUN: lld-link %t.obj -entry:main -debug:symtab -out:%t.exe
# RUN: llvm-objdump -d %t.exe | FileCheck %s --check-prefix=PLACED
# RUN: lld-link %t.norec.obj -entry:main -debug:symtab -out:%t.plain.exe
# RUN: llvm-objdump -d %t.plain.exe | FileCheck %s --check-prefix=PLAIN

## main fills .text up to 0x140002000, so h's entry would be at page offset 20,
## outside the first 16 bytes but inside the first 32.
# PLACED: 0000000140002024 <h>:
# PLAIN: 0000000140002014 <h>:

.ifndef NOREC
  .linktypeprefixes
.endif
        .def main; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,main
        .p2align 4
        .globl main
main:
        callq h
        retq
        .fill 4090, 1, 0xcc

        .def h; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,h
        .p2align 4
__cfi_h:
        .long 0x22222222
        nopl 0x71c5a06(%rax)
        movl $0x11111111, %eax
        .fill 4, 1, 0x90
        .globl h
h:
        retq

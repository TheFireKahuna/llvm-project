# REQUIRES: x86

## The linker places no entry of a function with a KCFI
## prefix in a page's first 16 bytes, where a KCFI check outside the code
## range reads no prefix and treats the target as foreign. A chunk that would
## put one there is moved by its alignment, whether the entry is the chunk's
## only one or one of several. Without an object
## that says it has KCFI prefixes, the layout is unchanged.

# RUN: llvm-mc -triple x86_64-windows-msvc %s -filetype=obj -o %t.obj
# RUN: llvm-mc -triple x86_64-windows-msvc %s -filetype=obj --defsym NOREC=1 \
# RUN:   -o %t.norec.obj
# RUN: lld-link %t.obj -entry:main -debug:symtab -out:%t.exe
# RUN: llvm-objdump -d %t.exe | FileCheck %s --check-prefix=PLACED
# RUN: lld-link %t.norec.obj -entry:main -debug:symtab -out:%t.plain.exe
# RUN: llvm-objdump -d %t.plain.exe | FileCheck %s --check-prefix=PLAIN

## main fills .text up to 0x140001ff0, so f's chunk would start there and its
## entry would be at 0x140002000. Once f has moved, g2's entry would be at
## 0x140003000.
# PLACED: 0000000140002010 <f>:
# PLACED: 0000000140002040 <g1>:
# PLACED: 0000000140003010 <g2>:
# PLAIN: 0000000140002000 <f>:
# PLAIN: 0000000140002020 <g1>:
# PLAIN: 0000000140002ff0 <g2>:

.ifndef NOREC
  .linktypeprefixes
.endif
        .def main; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,main
        .p2align 4
        .globl main
main:
        callq f
        callq g2
        retq
        .fill 4069, 1, 0xcc

        .def f; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,f
        .p2align 4
        .fill 4, 1, 0x90
__cfi_f:
        nopl 0x71c5a06(%rax)
        movl $0x11111111, %eax
        .globl f
f:
        retq

## Two functions in one section, the second 4048 bytes after the first's
## prefix.
        .section .text$mf,"xr"
        .p2align 4
        .def g1; .scl 2; .type 32; .endef
        .fill 4, 1, 0x90
__cfi_g1:
        nopl 0x71c5a06(%rax)
        movl $0x22222222, %eax
        .globl g1
g1:
        retq
        .fill 4031, 1, 0xcc
        .def g2; .scl 2; .type 32; .endef
        .fill 4, 1, 0x90
__cfi_g2:
        nopl 0x71c5a06(%rax)
        movl $0x33333333, %eax
        .globl g2
g2:
        retq

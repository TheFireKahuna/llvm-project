# REQUIRES: x86

## A patchable prefix between the KCFI type and the entry moves the prefix
## further from the entry, and a KCFI check then reads nothing before a target
## in the page's first PowerOf2Ceil(prefix + 12) bytes. Under -import-slots the
## linker keeps each entry out of that many bytes: here 8 bytes of patchable
## prefix keep h's entry out of the first 32.

# RUN: llvm-mc -triple x86_64-windows-msvc %s -filetype=obj -o %t.obj
# RUN: lld-link %t.obj -import-slots -entry:main -debug:symtab -out:%t.exe
# RUN: llvm-objdump -d %t.exe | FileCheck %s --check-prefix=SLOTS
# RUN: lld-link %t.obj -entry:main -debug:symtab -out:%t.plain.exe
# RUN: llvm-objdump -d %t.plain.exe | FileCheck %s --check-prefix=PLAIN

## main fills .text up to 0x140002000, so h's entry would be at page offset 20,
## outside the first 16 bytes but inside the first 32.
# SLOTS: 0000000140002024 <h>:
# PLAIN: 0000000140002014 <h>:

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
        nopl 0x71c5a06(%rax)
        movl $0x11111111, %eax
        .fill 8, 1, 0x90
        .globl h
h:
        retq

# REQUIRES: x86

## The pointers made for references to __imp_ symbols that the image defines
## are laid out by name, not in the order of the symbol table's hashing, which
## can differ between builds of the linker.

# RUN: llvm-mc -triple=x86_64-windows-msvc %s -filetype=obj -o %t.obj
# RUN: lld-link -entry:main -out:%t.exe %t.obj 2>/dev/null
# RUN: llvm-objdump -s -j .rdata %t.exe | FileCheck %s

# CHECK:      140002000 00300040 01000000 08300040 01000000
# CHECK-NEXT: 140002010 10300040 01000000 18300040 01000000
# CHECK-NEXT: 140002020 20300040 01000000

    .text
    .globl main
main:
    movq __imp_ccc(%rip), %rax
    movq __imp_aaa(%rip), %rax
    movq __imp_bbb(%rip), %rax
    movq __imp_eee(%rip), %rax
    movq __imp_ddd(%rip), %rax
    ret

    .data
    .globl aaa, bbb, ccc, ddd, eee
aaa: .quad 1
bbb: .quad 2
ccc: .quad 3
ddd: .quad 4
eee: .quad 5

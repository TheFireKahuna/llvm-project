# REQUIRES: x86

## Archive members that define the __imp_ symbols of automatically imported
## symbols load in the order of the symbols' names, not of the symbol table's
## hashing, so the image does not depend on how the linker was built.

# RUN: split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -triple=x86_64-windows-gnu main.s -filetype=obj -o main.obj
# RUN: llvm-mc -triple=x86_64-windows-gnu a.s -filetype=obj -o a.obj
# RUN: llvm-mc -triple=x86_64-windows-gnu b.s -filetype=obj -o b.obj
# RUN: llvm-mc -triple=x86_64-windows-gnu c.s -filetype=obj -o c.obj
# RUN: llvm-ar rcs lib.a c.obj b.obj a.obj
# RUN: lld-link -lldmingw -entry:main -out:out.exe main.obj lib.a -verbose 2>&1 \
# RUN:   | FileCheck %s

# CHECK:      Loading lazy __imp_aaa from lib.a for automatic import
# CHECK-NEXT: Loading lazy __imp_bbb from lib.a for automatic import
# CHECK-NEXT: Loading lazy __imp_ccc from lib.a for automatic import

#--- main.s
    .globl main
    .text
main:
    movl ccc(%rip), %eax
    movl aaa(%rip), %eax
    movl bbb(%rip), %eax
    ret
    .globl _pei386_runtime_relocator
_pei386_runtime_relocator:
    ret

#--- a.s
    .data
    .globl __imp_aaa
__imp_aaa:
    .quad 1

#--- b.s
    .data
    .globl __imp_bbb
__imp_bbb:
    .quad 2

#--- c.s
    .data
    .globl __imp_ccc
__imp_ccc:
    .quad 3

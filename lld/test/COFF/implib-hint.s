# REQUIRES: x86

# A member of the import library that imports by name carries the export's
# index in the DLL's export name table as its hint, whatever ordinal the export
# has. A NONAME export keeps its ordinal.

# RUN: split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc dll.s -o dll.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc main.s -o main.obj
# RUN: lld-link -dll -noentry dll.obj -out:dll.dll -implib:dll.lib \
# RUN:   -export:delta -export:alpha,@7 -export:echo -export:charlie \
# RUN:   -export:bravo -export:hidden,@5,NONAME
# RUN: llvm-readobj --coff-exports dll.dll | FileCheck --check-prefix=EXPORTS %s
# RUN: lld-link main.obj dll.lib -entry:main -out:main.exe
# RUN: llvm-readobj --coff-imports main.exe | FileCheck %s

# The name table holds alpha, bravo, charlie, delta and echo, in that order.
# EXPORTS: Name: alpha
# EXPORTS: Name: bravo
# EXPORTS: Name: charlie
# EXPORTS: Name: delta
# EXPORTS: Name: echo

# CHECK:      Name: dll.dll
# CHECK:      Symbol: alpha (0)
# CHECK-NEXT: Symbol: charlie (2)
# CHECK-NEXT: Symbol: echo (4)
# CHECK-NEXT: Symbol:  (5)

#--- dll.s
        .text
        .globl alpha, bravo, charlie, delta, echo, hidden
alpha:
bravo:
charlie:
delta:
echo:
hidden:
        retq

#--- main.s
        .text
        .globl main
main:
        callq *__imp_alpha(%rip)
        callq *__imp_charlie(%rip)
        callq *__imp_echo(%rip)
        callq *__imp_hidden(%rip)
        retq

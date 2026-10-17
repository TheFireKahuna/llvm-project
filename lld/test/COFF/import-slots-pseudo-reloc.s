# REQUIRES: x86
# RUN: llvm-mc -triple x86_64-windows-gnu %s -filetype=obj -o %t.obj

# -import-slots leaves no fixups for pseudo relocations to make, so asking for
# them is an error, and MinGW mode no longer turns them on by default.
# RUN: not lld-link -lldmingw -import-slots -runtime-pseudo-reloc %t.obj \
# RUN:   -entry:main -out:%t.exe 2>&1 | FileCheck %s
# RUN: not lld-link -import-slots -runtime-pseudo-reloc %t.obj -entry:main \
# RUN:   -out:%t.exe 2>&1 | FileCheck %s
# RUN: lld-link -lldmingw -import-slots %t.obj -entry:main -out:%t.exe
# RUN: lld-link -import-slots -runtime-pseudo-reloc -runtime-pseudo-reloc:no \
# RUN:   %t.obj -entry:main -out:%t.exe

# CHECK: error: -runtime-pseudo-reloc is not compatible with -import-slots

        .text
        .globl main
main:
        retq

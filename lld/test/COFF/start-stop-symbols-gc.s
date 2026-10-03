# REQUIRES: x86

## A reference to __start_X or __stop_X does not keep the sections of run X,
## as under ELF's -z start-stop-gc: an unreferenced COMDAT section of the run
## is collected. A run named __libc_* is kept whole while its bounds are
## referenced.

# RUN: split-file %s %t.dir
# RUN: llvm-mc -triple x86_64-windows-msvc %t.dir/main.s -filetype=obj -o %t.main.obj
# RUN: llvm-mc -triple x86_64-windows-msvc %t.dir/libc.s -filetype=obj -o %t.libc.obj

# RUN: lld-link %t.main.obj -entry:main -start-stop-symbols -opt:ref \
# RUN:   -out:%t.exe
# RUN: llvm-objdump -s --section=myset %t.exe | FileCheck %s --check-prefix=GONE
# GONE:      Contents of section myset:
# GONE-NEXT: 140003000 00000000 00000000 ........{{$}}

# RUN: lld-link %t.libc.obj -entry:main -start-stop-symbols -opt:ref \
# RUN:   -out:%t.libc.exe
# RUN: llvm-objdump -s --section=__libc_a %t.libc.exe | FileCheck %s --check-prefix=KEPT
# KEPT: 140003000 00000000 00000000 01000000 00000000

#--- main.s
        .globl main
        .text
main:
        retq

        .data
        .quad __start_myset
        .quad __stop_myset

        .section myset,"dr"
        .quad 0
        .section myset$a,"dr",discard,entry1
        .globl entry1
entry1:
        .quad 1

#--- libc.s
        .globl main
        .text
main:
        retq

        .data
        .quad __start___libc_a
        .quad __stop___libc_a

        .section __libc_a,"dr"
        .quad 0
        .section __libc_a$a,"dr",discard,entry1
        .globl entry1
entry1:
        .quad 1

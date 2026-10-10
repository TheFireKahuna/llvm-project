# REQUIRES: x86

## With -start-stop-symbols, a referenced, undefined __start_X or __stop_X,
## where X is a C identifier, bounds the input sections named X or X$*.

# RUN: split-file %s %t.dir
# RUN: llvm-mc -triple x86_64-windows-msvc %t.dir/main.s -filetype=obj -o %t.main.obj
# RUN: llvm-mc -triple x86_64-windows-msvc %t.dir/none.s -filetype=obj -o %t.none.obj
# RUN: llvm-mc -triple x86_64-windows-msvc %t.dir/split.s -filetype=obj -o %t.split.obj
# RUN: llvm-mc -triple x86_64-windows-msvc %t.dir/defined.s -filetype=obj -o %t.defined.obj

## myset, myset$a and myset$b sort into one output section, and the bounds
## enclose all three.
# RUN: lld-link %t.main.obj -entry:main -start-stop-symbols -debug:symtab \
# RUN:   -out:%t.exe
# RUN: llvm-objdump -s -t %t.exe | FileCheck %s
# CHECK:      (sec  3){{.*}} 0x00000000 __start_myset
# CHECK:      (sec  3){{.*}} 0x00000018 __stop_myset
# CHECK:      Contents of section myset:
# CHECK-NEXT: 140003000 00000000 00000000 01000000 00000000
# CHECK-NEXT: 140003010 02000000 00000000 ........

## Weak references are bounded as ELF bounds them; with no section to bound,
## they keep their zero default.
# RUN: llvm-mc -triple x86_64-windows-msvc %t.dir/weak.s -filetype=obj -o %t.weak.obj
# RUN: lld-link %t.weak.obj -entry:main -start-stop-symbols -debug:symtab \
# RUN:   -out:%t.weak.exe
# RUN: llvm-objdump -s -t %t.weak.exe | FileCheck %s --check-prefix=WEAK
# WEAK:      (sec  3){{.*}} 0x00000000 __start_myset
# WEAK:      (sec  3){{.*}} 0x00000008 __stop_myset
# WEAK:      Contents of section .data:
# WEAK-NEXT: 140002000 00300040 01000000 08300040 01000000
# WEAK-NEXT: 140002010 00000000 00000000

## The symbols are not defined by default, or with -start-stop-symbols:no.
# RUN: not lld-link %t.main.obj -entry:main -out:%t.off.exe 2>&1 \
# RUN:   | FileCheck %s --check-prefix=OFF
# RUN: not lld-link %t.main.obj -entry:main -start-stop-symbols \
# RUN:   -start-stop-symbols:no -out:%t.off.exe 2>&1 \
# RUN:   | FileCheck %s --check-prefix=OFF
# OFF: undefined symbol: __start_myset

## With no section to bound, the reference stays undefined.
# RUN: not lld-link %t.none.obj -entry:main -start-stop-symbols \
# RUN:   -out:%t.none.exe 2>&1 | FileCheck %s --check-prefix=NONE
# NONE: undefined symbol: __start_absent

## Sections of the run in two output sections cannot be bounded.
# RUN: not lld-link %t.split.obj -entry:main -start-stop-symbols \
# RUN:   -out:%t.split.exe 2>&1 | FileCheck %s --check-prefix=SPLIT
# SPLIT: error: section myset is split across output sections and cannot be bounded by __start_myset and __stop_myset

## A bound that an input defines is not paired with one the linker defines.
# RUN: not lld-link %t.defined.obj -entry:main -start-stop-symbols \
# RUN:   -out:%t.defined.exe 2>&1 | FileCheck %s --check-prefix=DEFINED
# DEFINED: error: __start_myset cannot be defined by the linker: __stop_myset is defined by an input

#--- main.s
        .globl main
        .text
main:
        retq

        .data
        .quad __start_myset
        .quad __stop_myset

        .section myset$b,"dr"
        .quad 2
        .section myset,"dr"
        .quad 0
        .section myset$a,"dr"
        .quad 1

#--- none.s
        .globl main
        .text
main:
        retq

        .data
        .quad __start_absent

#--- split.s
        .globl main
        .text
main:
        retq

        .data
        .quad __start_myset
        .quad __stop_myset

        .section myset,"dr"
        .quad 0
        .section myset$a,"dw"
        .quad 1

#--- defined.s
        .globl main
        .text
main:
        retq

        .data
        .quad __start_myset

        .section myset,"dr"
        .globl __stop_myset
        .quad 0
__stop_myset:

#--- weak.s
        .globl main
        .text
main:
        retq

        .weak __start_myset
        .weak __stop_myset
        .weak __start_absent
        .data
        .quad __start_myset
        .quad __stop_myset
        .quad __start_absent

        .section myset,"dr"
        .quad 1

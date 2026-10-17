# REQUIRES: x86
## An executable that defines __wincrt_exit_state, the process's exit-list
## state that the C runtime's start-up reserves, names it as {tag, rva} at 0x30
## in its MZ header, where every copy of the runtime's termination registries
## finds it through PEB->ImageBaseAddress. A DLL names none, nor does an
## executable without it.

# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium %s -o %t.obj
# RUN: lld-link -entry:main -subsystem:console %t.obj -out:%t.exe
# RUN: llvm-readobj --sections %t.exe > %t.txt
# RUN: od -A x -t x4 -j 0x30 -N 8 %t.exe >> %t.txt
# RUN: FileCheck --input-file=%t.txt %s
# CHECK:      Name: .data
# CHECK-NEXT: VirtualSize:
# CHECK-NEXT: VirtualAddress: 0x[[#%X,DATA:]]
# CHECK:      000030 7e1a5c03 {{0*}}[[#%x,DATA]]

# RUN: lld-link -dll -noentry %t.obj -out:%t.dll
# RUN: od -A x -t x4 -j 0x30 -N 8 %t.dll | FileCheck --check-prefix=NONE %s
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium --defsym=NOSTATE=1 \
# RUN:   %s -o %t.none.obj
# RUN: lld-link -entry:main -subsystem:console %t.none.obj -out:%t.none.exe
# RUN: od -A x -t x4 -j 0x30 -N 8 %t.none.exe | FileCheck --check-prefix=NONE %s
# NONE: 000030 00000000 00000000

        .text
        .globl main
main:
        retq
.ifndef NOSTATE
        .data
        .globl __wincrt_exit_state
__wincrt_exit_state:
        .long 1
        .long 0
.endif

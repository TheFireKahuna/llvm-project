# REQUIRES: x86

## The loader reads the TLS directory only when binding imports, so an image
## with thread-local data and no import states one zero word as its IAT.

# RUN: llvm-mc -triple=x86_64-windows-itanium -filetype=obj %s -o %t.obj
# RUN: llvm-mc -triple=x86_64-windows-itanium -filetype=obj \
# RUN:   %S/Inputs/tls-support.s -o %t.tls.obj
# RUN: lld-link -dll -noentry %t.obj %t.tls.obj -out:%t.dll
# RUN: llvm-readobj --file-headers %t.dll | FileCheck %s --check-prefix=GATE
# RUN: lld-link -entry:main -subsystem:console %t.obj %t.tls.obj -out:%t.exe
# RUN: llvm-readobj --file-headers %t.exe | FileCheck %s --check-prefix=GATE
# RUN: lld-link -dll -noentry %t.obj -out:%t.none.dll
# RUN: llvm-readobj --file-headers %t.none.dll | FileCheck %s --check-prefix=NONE

# GATE: ImportTableRVA: 0x0
# GATE: TLSTableRVA: 0x2000
# GATE: IATRVA: 0x2028
# GATE: IATSize: 0x8

# NONE: TLSTableRVA: 0x0
# NONE: IATRVA: 0x0

        .text
        .globl  main
main:
        retq

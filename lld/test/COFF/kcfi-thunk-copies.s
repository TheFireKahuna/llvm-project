# REQUIRES: x86

## The copies of a KCFI thunk's COMDAT that objects define are interchangeable
## only if they compare the same prefixes. A copy whose record gives another
## marker, or another patchable prefix, is an error, whichever copy the link
## keeps.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc --defsym MARKER=0x071c5a06 --defsym OFFSET=0 thunk.s -o a.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc --defsym MARKER=0x071c5a06 --defsym OFFSET=0 thunk.s -o same.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc --defsym MARKER=0x12345678 --defsym OFFSET=0 thunk.s -o marker.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc --defsym MARKER=0x071c5a06 --defsym OFFSET=4 thunk.s -o offset.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc main.s -o main.obj

# RUN: lld-link main.obj a.obj same.obj -entry:main -out:same.exe
# RUN: not lld-link main.obj a.obj marker.obj -entry:main -out:marker.exe 2>&1 \
# RUN:   | FileCheck %s --check-prefix=MARKER
# RUN: not lld-link main.obj a.obj offset.obj -entry:main -out:offset.exe 2>&1 \
# RUN:   | FileCheck %s --check-prefix=OFFSET

# MARKER: error: marker.obj: KCFI thunk __llvm_kcfi_dispatch_11111111 has marker 0x12345678 and offset 0, but its copy in a.obj has marker 0x71c5a06 and offset 0
# OFFSET: error: offset.obj: KCFI thunk __llvm_kcfi_dispatch_11111111 has marker 0x71c5a06 and offset 4, but its copy in a.obj has marker 0x71c5a06 and offset 0

#--- main.s
        .globl main
main:
        callq __llvm_kcfi_dispatch_11111111
        retq

#--- thunk.s
        .weak __llvm_kcfi_mismatch_11111111
__llvm_kcfi_mismatch_11111111 = __llvm_kcfi_trap

        .section .text,"xr",discard,__llvm_kcfi_dispatch_11111111
        .globl __llvm_kcfi_dispatch_11111111
        .p2align 4
__llvm_kcfi_dispatch_11111111:
        .linkkcfithunk __llvm_kcfi_dispatch_11111111, dispatch, 0x11111111, MARKER, OFFSET, __llvm_kcfi_mismatch_11111111
        jne __llvm_kcfi_mismatch_11111111
        jmpq *%rax

        .section .text,"xr",discard,__llvm_kcfi_trap
        .globl __llvm_kcfi_trap
        .p2align 4
__llvm_kcfi_trap:
        int3

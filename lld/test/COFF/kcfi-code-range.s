# REQUIRES: x86

## In an image it seals, the linker defines __llvm_code_start and
## __llvm_code_end as the bounds of the output section that holds the KCFI
## prefixes. Otherwise they keep clang's weak default, one byte in a COMDAT, an
## empty range.

# RUN: split-file %s %t.dir
# RUN: llvm-mc -triple x86_64-windows-msvc %t.dir/main.s -filetype=obj -o %t.main.obj
# RUN: llvm-mc -triple x86_64-windows-msvc %t.dir/other.s -filetype=obj -o %t.other.obj

## .text holds f's chunk at 0x1000 and main's, 6 bytes, at 0x1020, so the
## range [0x140001000, 0x140001026) encloses the prefix and the function.
# RUN: lld-link %t.main.obj -guard:cf -import-slots -entry:main -out:%t.exe
# RUN: llvm-objdump -s --section=.data %t.exe | FileCheck %s --check-prefix=SEALED
# SEALED: {{^ [0-9a-f]+}} 00100040 01000000 26100040 01000000

## Without -import-slots or without a guard function table, the image is not
## sealed.
# RUN: lld-link %t.main.obj -guard:cf -entry:main -out:%t.noslots.exe
# RUN: llvm-objdump -s --section=.data %t.noslots.exe | FileCheck %s --check-prefix=EMPTY
# RUN: lld-link %t.main.obj -import-slots -entry:main -out:%t.nocfg.exe
# RUN: llvm-objdump -s --section=.data %t.nocfg.exe | FileCheck %s --check-prefix=EMPTY
# EMPTY: {{^ [0-9a-f]+}} [[LO:[0-9a-f]+]] [[HI:[0-9a-f]+]] [[LO]] [[HI]]

## Prefixes in two output sections leave the range empty.
# RUN: lld-link %t.main.obj %t.other.obj -guard:cf -import-slots -entry:main \
# RUN:   -out:%t.two.exe
# RUN: llvm-objdump -s --section=.data %t.two.exe | FileCheck %s --check-prefix=EMPTY

#--- main.s
        .globl @feat.00
@feat.00 = 0x800

        .def f; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,f
        .p2align 4
        .fill 4, 1, 0x90
__cfi_f:
        nopl 0x71c5a06(%rax)
        movl $0x11111111, %eax
        .globl f
f:
        retq

        .def main; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,main
        .p2align 4
        .globl main
main:
        callq f
        retq

        .weak __llvm_code_start
__llvm_code_start = __llvm_code_empty
        .weak __llvm_code_end
__llvm_code_end = __llvm_code_empty
        .section .rdata,"dr",discard,__llvm_code_empty
        .globl __llvm_code_empty
__llvm_code_empty:
        .byte 0

        .data
        .quad __llvm_code_start
        .quad __llvm_code_end

#--- other.s
        .globl @feat.00
@feat.00 = 0x800

        .def g; .scl 2; .type 32; .endef
        .section .xtext,"xr",one_only,g
        .p2align 4
        .fill 4, 1, 0x90
__cfi_g:
        nopl 0x71c5a06(%rax)
        movl $0x22222222, %eax
        .globl g
g:
        retq

        .section .xtext,"xr"
        .globl g_user
g_user:
        jmp g

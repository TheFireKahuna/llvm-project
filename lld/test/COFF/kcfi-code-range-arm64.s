# REQUIRES: aarch64

## On ARM64, as on x86-64, the linker bounds the output section that holds the
## KCFI prefixes of an image it seals with __llvm_code_start and
## __llvm_code_end: .text holds f's chunk at 0x1000 and main's, 8 bytes, at
## 0x1010.

# RUN: llvm-mc -triple aarch64-windows-msvc %s -filetype=obj -o %t.obj
# RUN: lld-link %t.obj -machine:arm64 -guard:cf -import-slots -entry:main \
# RUN:   -out:%t.exe
# RUN: llvm-objdump -s --section=.data %t.exe | FileCheck %s --check-prefix=SEALED
# SEALED: {{^ [0-9a-f]+}} 00100040 01000000 18100040 01000000

# RUN: lld-link %t.obj -machine:arm64 -guard:cf -entry:main -out:%t.noslots.exe
# RUN: llvm-objdump -s --section=.data %t.noslots.exe | FileCheck %s --check-prefix=EMPTY
# EMPTY: {{^ [0-9a-f]+}} [[LO:[0-9a-f]+]] [[HI:[0-9a-f]+]] [[LO]] [[HI]]

        .globl @feat.00
@feat.00 = 0x800

        .def f; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,f
        .p2align 4
__cfi_f:
        .byte 0x0f, 0x1f, 0x80
        .long 0x071c5a06
        .byte 0xb8
        .long 0x11111111
        .globl f
f:
        ret

        .def main; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,main
        .p2align 4
        .globl main
main:
        bl f
        ret

        .weak __llvm_code_start
__llvm_code_start = __llvm_code_empty
        .weak __llvm_code_end
__llvm_code_end = __llvm_code_empty
        .section .rdata,"dr",discard,__llvm_code_empty
        .globl __llvm_code_empty
__llvm_code_empty:
        .byte 0

        .data
        .xword __llvm_code_start
        .xword __llvm_code_end

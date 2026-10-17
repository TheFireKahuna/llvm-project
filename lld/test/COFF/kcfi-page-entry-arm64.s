# REQUIRES: aarch64

## On ARM64 the linker keeps entries of functions with a KCFI prefix out of a
## page's first 16 bytes too, moving a chunk by its alignment until none is
## there, even where that alignment is less than 16.

# RUN: llvm-mc -triple aarch64-windows-msvc %s -filetype=obj -o %t.obj
# RUN: llvm-mc -triple aarch64-windows-msvc %s -filetype=obj --defsym NOREC=1 \
# RUN:   -o %t.norec.obj
# RUN: lld-link %t.obj -machine:arm64 -entry:main -debug:symtab \
# RUN:   -out:%t.exe
# RUN: llvm-objdump -d %t.exe | FileCheck %s --check-prefix=PLACED
# RUN: lld-link %t.norec.obj -machine:arm64 -entry:main -debug:symtab \
# RUN:   -out:%t.plain.exe
# RUN: llvm-objdump -d %t.plain.exe | FileCheck %s --check-prefix=PLAIN

## main ends at 0x140001ff8, so f's entry would be at 0x140002004.
# PLACED: 0000000140002010 <f>:
# PLAIN: 0000000140002004 <f>:

.ifndef NOREC
  .linktypeprefixes
.endif
        .def main; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,main
        .p2align 4
        .globl main
main:
        bl f
        ret
        .fill 4080, 1, 0

        .def f; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,f
        .p2align 2
__cfi_f:
        .byte 0x0f, 0x1f, 0x80
        .long 0x071c5a06
        .byte 0xb8
        .long 0x11111111
        .globl f
f:
        ret

# REQUIRES: x86

## The map files, the symbol table and the PDB describe a chunk whose KCFI
## prefix the image leaves out from its entry, the first byte the image holds
## of it, and leave out the symbols in the bytes it does not hold.

# RUN: llvm-mc -triple x86_64-windows-msvc %s -filetype=obj -o %t.obj
# RUN: lld-link %t.obj -guard:cf -entry:main -debug \
# RUN:   -debug:symtab -pdb:%t.pdb -map:%t.map -lldmap:%t.lldmap -out:%t.exe
# RUN: FileCheck %s --check-prefix=MAP < %t.map
# RUN: FileCheck %s --check-prefix=LLDMAP < %t.lldmap
# RUN: llvm-nm %t.exe | FileCheck %s --check-prefix=SYMTAB
# RUN: llvm-pdbutil dump -section-contribs %t.pdb \
# RUN:   | FileCheck %s --check-prefix=CONTRIBS
# RUN: llvm-pdbutil dump -symbols %t.pdb | FileCheck %s --check-prefix=GROUPS

## main is 16 bytes, so a's entry is at 0x140001010, its prefix over main.
# MAP:      0001:00000000 00000010H .text                   CODE
# MAP-NEXT: 0001:00000010 00000003H .text$mn                CODE
# MAP-NOT:  __cfi_a
# MAP:      Static symbols
# MAP-NOT:  __cfi_a

# LLDMAP:      00001010 00000003    16         {{.*}}.obj:(.text$mn)
# LLDMAP-NEXT: 00001010 00000000     0                 a
# LLDMAP-NOT:  __cfi_a

# SYMTAB-NOT: __cfi_a
# SYMTAB:     140001010 T a
# SYMTAB-NOT: __cfi_a

# CONTRIBS:      SC[.text]   | mod = 0, 0001:0000, size = 16,
# CONTRIBS-NEXT:   IMAGE_SCN_CNT_CODE
# CONTRIBS-NEXT:   IMAGE_SCN_MEM_EXECUTE
# CONTRIBS-NEXT: SC[.text]   | mod = 0, 0001:0016, size = 3,

# GROUPS:      S_COFFGROUP [size = {{[0-9]+}}] `.text`
# GROUPS-NEXT:   length = 16, addr = 0001:0000
# GROUPS:      S_COFFGROUP [size = {{[0-9]+}}] `.text$mn`
# GROUPS-NEXT:   length = 3, addr = 0001:0016

  .linktypeprefixes
        .globl @feat.00
@feat.00 = 0x800

        .def main; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,main
        .p2align 4
        .globl main
main:
        callq a
        retq
        .fill 10, 1, 0xcc

        .def a; .scl 2; .type 32; .endef
        .section .text$mn,"xr",one_only,a
        .p2align 4
        .fill 4, 1, 0x90
__cfi_a:
        nopl 0x71c5a06(%rax)
        movl $0x11111111, %eax
        .globl a
a:
        pushq %rbx
        popq %rbx
        retq

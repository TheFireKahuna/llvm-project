# REQUIRES: x86

## Under -import-slots the linker lays out the pinned and 64-byte-aligned
## chunks of .rdata so that the padding before each holds other chunks of
## .rdata, largest first, rather than zeros. Without -import-slots the order
## of .rdata is its input order, and only the pins' padding is added.

# RUN: llvm-mc -triple x86_64-unknown-windows-itanium %s -filetype=obj -o %t.obj
# RUN: lld-link %t.obj -import-slots -entry:main -debug:symtab -out:%t.exe
# RUN: llvm-nm -n %t.exe | FileCheck %s --check-prefix=PACK
# RUN: llvm-readobj --sections %t.exe | FileCheck %s --check-prefix=PACKSIZE
# RUN: lld-link %t.obj -entry:main -debug:symtab -out:%t.plain.exe
# RUN: llvm-nm -n %t.plain.exe | FileCheck %s --check-prefix=PLAIN
# RUN: llvm-readobj --sections %t.plain.exe | \
# RUN:   FileCheck %s --check-prefix=PLAINSIZE

## a64's 40 bytes of padding hold s40; b64 follows on the next line, and the
## padding before the pin at 216 holds s24, s16 and s8 in turn.
# PACK:      140002000 r a64
# PACK-NEXT: 140002018 r s40
# PACK-NEXT: 140002040 r b64
# PACK-NEXT: 140002068 r s24
# PACK-NEXT: 140002080 r s16
# PACK-NEXT: 140002090 r s8
# PACK-NEXT: 1400020d8 r pinned
# PACKSIZE:      Name: .rdata
# PACKSIZE-NEXT: VirtualSize: 0xE8

# PLAIN:      140002000 r s8
# PLAIN-NEXT: 140002040 r a64
# PLAIN-NEXT: 140002058 r s16
# PLAIN-NEXT: 140002080 r b64
# PLAIN-NEXT: 1400020a8 r s24
# PLAIN-NEXT: 1400020d8 r pinned
# PLAIN-NEXT: 1400020e8 r s40
# PLAINSIZE:      Name: .rdata
# PLAINSIZE-NEXT: VirtualSize: 0x110

  .text
  .globl main
main:
  leaq a64(%rip), %rax
  leaq b64(%rip), %rax
  leaq pinned(%rip), %rax
  leaq s8(%rip), %rax
  leaq s16(%rip), %rax
  leaq s24(%rip), %rax
  leaq s40(%rip), %rax
  retq

  .section .rdata,"dr",one_only,s8
  .p2align 3
s8:
  .quad 0

  .section .rdata,"dr",one_only,a64
  .p2align 6
a64:
  .quad 0, 0, 0

  .section .rdata,"dr",one_only,s16
  .p2align 3
s16:
  .quad 0, 0

  .section .rdata,"dr",one_only,b64
  .p2align 6
b64:
  .quad 0, 0, 0, 0, 0

  .section .rdata,"dr",one_only,s24
  .p2align 3
s24:
  .quad 0, 0, 0

  .section .rdata,"dr",one_only,pinned
  .p2align 3
pinned:
  .quad 0, 0
  .linkpin pinned, 12, 216, required

  .section .rdata,"dr",one_only,s40
  .p2align 3
s40:
  .quad 0, 0, 0, 0, 0

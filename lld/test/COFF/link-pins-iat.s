# REQUIRES: x86

## Under -import-slots, a pinned or 64-byte-aligned chunk holding an in-place
## import slot is held back into the import address table directory, at the
## start of .rdata, and packed there: the padding before it holds chunks of
## plain .rdata, which move into the directory, rather than zeros. The
## directory spans the chunks that fill its gaps.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-windows-itanium main.s -o main.obj
# RUN: lld-link -def:a.def -out:a.lib -machine:x64
# RUN: lld-link -import-slots -entry:main -subsystem:console main.obj a.lib \
# RUN:   -debug:symtab -out:out.exe
# RUN: llvm-nm -n out.exe | FileCheck %s
# RUN: llvm-readobj --file-headers out.exe | FileCheck --check-prefix=DIR %s

## Packing can empty plain .rdata into the directory; a PDB still links.
# RUN: lld-link -import-slots -entry:main -subsystem:console main.obj a.lib \
# RUN:   -debug -out:debug.exe

## The address tables end at 0x18; s32 and s8 fill the padding before vt1, s16
## follows it, and vt2 takes its pin at 3832. The directory reaches vt2's end.
# CHECK:      140002000 R __imp_func1
# CHECK-NEXT: 140002008 R __imp_func2
# CHECK-NEXT: 140002018 r s32
# CHECK-NEXT: 140002038 r s8
# CHECK-NEXT: 140002040 R vt1
# CHECK-NEXT: 140002050 r s16
# CHECK-NEXT: 140002ef8 R vt2
# DIR:      IATRVA: 0x2000
# DIR-NEXT: IATSize: 0xF10

#--- a.def
LIBRARY a.dll
EXPORTS
  func1
  func2

#--- main.s
  .text
  .globl main
main:
  movq vt1(%rip), %rax
  movq vt2(%rip), %rax
  movq s8(%rip), %rax
  movq s16(%rip), %rax
  movq s32(%rip), %rax
  retq

  .section .rdata,"dr",one_only,vt1
  .p2align 6
  .globl vt1
vt1:
  .quad 0
  .quad func1

  .section .rdata,"dr",one_only,vt2
  .p2align 3
  .globl vt2
vt2:
  .quad 0, 0
  .quad func2
  .linkpin vt2, 12, 3832, required

  .section .rdata,"dr",one_only,s8
  .p2align 3
s8:
  .quad 0

  .section .rdata,"dr",one_only,s16
  .p2align 3
s16:
  .quad 0, 0

  .section .rdata,"dr",one_only,s32
  .p2align 3
s32:
  .quad 0, 0, 0, 0

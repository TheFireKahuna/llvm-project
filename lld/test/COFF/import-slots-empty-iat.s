# REQUIRES: x86
## Under -import-slots, when every import is kept only in in-place slots, the
## image has no import address table entries, and the import address table
## directory spans only the read-only slots: from the chunk that packing puts
## first, from the first $-group of .rdata when only $-groups hold them, and
## none at all when every slot is writable.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: lld-link -def:a.def -out:a.lib -machine:x64
# RUN: lld-link -def:b.def -out:b.lib -machine:x64

## The pinned vt is packed before small, which preceded it.
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc packed.s -o packed.obj
# RUN: lld-link -import-slots -opt:noref -entry:main -subsystem:console \
# RUN:   packed.obj a.lib b.lib -debug:symtab -out:packed.exe
# RUN: llvm-readobj --file-headers packed.exe | \
# RUN:   FileCheck --check-prefix=PACKED %s
# RUN: llvm-nm -n packed.exe | FileCheck --check-prefix=PACKED-SYMS %s

# PACKED:      IATRVA: 0x2000
# PACKED-NEXT: IATSize: 0x18
# PACKED-SYMS:      140002000 R vt
# PACKED-SYMS-NEXT: 140002008 R __imp_fb
# PACKED-SYMS-DAG:  140002010 R __imp_fa
# PACKED-SYMS-DAG:  140002010 R small

# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc group.s -o group.obj
# RUN: lld-link -import-slots -opt:noref -entry:main -subsystem:console \
# RUN:   group.obj a.lib -out:group.exe
# RUN: llvm-readobj --file-headers --coff-imports group.exe | \
# RUN:   FileCheck --check-prefix=GROUP %s

# GROUP:      IATRVA: 0x2000
# GROUP-NEXT: IATSize: 0x8
# GROUP:      ImportAddressTableRVA: 0x2000
# GROUP-NEXT: Symbol: fa (0)

# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc writable.s \
# RUN:   -o writable.obj
# RUN: lld-link -import-slots -opt:noref -entry:main -subsystem:console \
# RUN:   writable.obj a.lib -out:writable.exe
# RUN: llvm-readobj --file-headers --coff-imports writable.exe | \
# RUN:   FileCheck --check-prefix=WRITABLE %s

# WRITABLE:      IATRVA: 0x0
# WRITABLE-NEXT: IATSize: 0x0
# WRITABLE:      ImportAddressTableRVA: 0x3000
# WRITABLE-NEXT: Symbol: fa (0)

#--- a.def
LIBRARY a.dll
EXPORTS
  fa

#--- b.def
LIBRARY b.dll
EXPORTS
  fb

#--- packed.s
  .text
  .globl main
main:
  retq

  .section .rdata,"dr",one_only,small
  .p2align 3
  .globl small
small:
  .quad fa

  .section .rdata,"dr",one_only,vt
  .p2align 6
  .globl vt
vt:
  .quad 0
  .quad fb
  .linkpin vt, 6, 0

#--- group.s
  .text
  .globl main
main:
  retq

  .section .rdata$zz,"dr"
  .p2align 3
  .quad fa

#--- writable.s
  .text
  .globl main
main:
  retq

  .data
  .quad fa

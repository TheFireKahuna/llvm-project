# REQUIRES: x86

## A DLL exports the run of each class whose vtable or type_info it exports as
## three words: the run's start, under the name of the class's address point,
## and its end and its start plus the stride less one, under private names
## hashed from the class. Its import library imports each word under the
## range symbol's name and the DLL's.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium dll.s -o dll.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium main.s -o main.obj
# RUN: lld-link -dll -noentry -import-slots dll.obj -out:lib.dll \
# RUN:   -implib:lib.lib -debug:symtab
# RUN: llvm-nm --numeric-sort lib.dll > dll.txt
# RUN: llvm-readobj --coff-exports lib.dll >> dll.txt
# RUN: FileCheck %s --check-prefix=EXPORTS < dll.txt
# RUN: llvm-nm lib.lib | FileCheck %s --check-prefix=IMPLIB

## B's run holds B and C, two 64-byte slots. U's vtable is not exported, and
## its run is not either.
# EXPORTS:      [[#%x,B:]] R _ZTV1B
# EXPORTS:      Name: _ZTV1B$ap16
# EXPORTS-NEXT: RVA: 0x[[#%X,B-6442450944+16]]
# EXPORTS:      Name: _ZTV1C$ap16
# EXPORTS-NEXT: RVA: 0x[[#%X,B-6442450944+16+64]]
# EXPORTS:      Name: __llvm_vtr$0HTFUliAe5se
# EXPORTS-NEXT: RVA: 0x[[#%X,B-6442450944+16+128]]
# EXPORTS:      Name: __llvm_vtr$0HTFUliAe5sm
# EXPORTS-NEXT: RVA: 0x[[#%X,B-6442450944+16+63]]
# EXPORTS:      Name: __llvm_vtr$3XOjuGn_5Uke
# EXPORTS-NEXT: RVA: 0x[[#%X,B-6442450944+16+128]]
# EXPORTS:      Name: __llvm_vtr$3XOjuGn_5Ukm
# EXPORTS-NEXT: RVA: 0x[[#%X,B-6442450944+16+64+63]]
# EXPORTS-NOT:  _ZTV1U$ap16

# IMPLIB-DAG: __imp___llvm_vtables$1B$lib.dll
# IMPLIB-DAG: __imp___llvm_vtables_end$1B$lib.dll
# IMPLIB-DAG: __imp___llvm_vtables_mask$1B$lib.dll
# IMPLIB-DAG: __imp___llvm_vtables$1C$lib.dll
# IMPLIB-NOT: __llvm_vtables{{.*}}$1U$

## An importer that imports B's type_info from the DLL binds B's run there, as
## three consecutive import address table entries that its owner words name.
## C's type_info it does not import, and Z has no run in any DLL, so both read
## the shared zero words, an empty range.
# RUN: lld-link -import-slots -entry:main -subsystem:console main.obj lib.lib \
# RUN:   -debug:symtab -out:main.exe
# RUN: llvm-readobj --coff-imports main.exe > main.txt
# RUN: llvm-nm --numeric-sort main.exe >> main.txt
# RUN: FileCheck %s --check-prefix=OWNER < main.txt

# OWNER:      ImportAddressTableRVA: 0x[[#%X,IAT:]]
# OWNER:      Symbol: _ZTI1B
# OWNER-NEXT: Symbol: _ZTV1B$ap16
# OWNER-NEXT: Symbol: __llvm_vtr$0HTFUliAe5se
# OWNER-NEXT: Symbol: __llvm_vtr$0HTFUliAe5sm

## L, derived from B, has no B vtable beside it, yet B's run is L's slot, so
## that a check of B finds the objects of a class the image derives from the
## DLL's.
# OWNER:      [[#%x,L:]] R _ZTV1L
# OWNER:      [[#L+16]] R __llvm_vtables$1B
# OWNER:      [[#IAT+5368709120+8]] R __llvm_vtables_owner$1B
# OWNER:      [[#%x,EMPTY:]] R __llvm_vtables_owner$1C
# OWNER-NEXT: [[#EMPTY]] R __llvm_vtables_owner$1Z

#--- dll.s
  .section .drectve,"yni"
  .ascii " -export:_ZTV1B,DATA -export:_ZTV1C,DATA -export:_ZTI1B,DATA"

  .section .rdata,"dr",discard,_ZTV1B
  .globl _ZTV1B
  .p2align 6
_ZTV1B:
  .quad 0
  .quad _ZTI1B
  .quad f
  .linkvtable _ZTV1B, 16, "1B", ""

  .section .rdata,"dr",discard,_ZTV1C
  .globl _ZTV1C
  .p2align 6
_ZTV1C:
  .quad 0
  .quad 0
  .quad f
  .linkvtable _ZTV1C, 16, "1C", "1B"

  .section .rdata,"dr",discard,_ZTV1U
  .globl _ZTV1U
  .p2align 6
_ZTV1U:
  .quad 0
  .quad 0
  .quad f
  .linkvtable _ZTV1U, 16, "1U", ""

  .section .rdata,"dr",discard,_ZTI1B
  .globl _ZTI1B
_ZTI1B:
  .quad 0

  .text
  .globl f
f:
  retq

#--- main.s
  .text
  .globl main
main:
  leaq _ZTV1L(%rip), %rax
  movq __imp__ZTI1B(%rip), %rax
  movq __llvm_vtables$1B(%rip), %rax
  movq __llvm_vtables_owner$1B(%rip), %rax
  movq __llvm_vtables_owner$1C(%rip), %rax
  movq __llvm_vtables_owner$1Z(%rip), %rax
  retq

  .section .rdata,"dr",discard,_ZTV1L
  .globl _ZTV1L
  .p2align 6
_ZTV1L:
  .quad 0
  .quad 0
  .quad g
  .linkvtable _ZTV1L, 16, "1L", "1B"

  .text
  .globl g
g:
  retq

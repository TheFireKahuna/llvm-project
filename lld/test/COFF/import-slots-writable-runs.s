# REQUIRES: x86
## Under -import-slots, the writable chunks of .data that hold in-place import
## slots are laid out together at its end, ordered by the DLL of their first
## slot, so that single-pointer chunks of one DLL form one run with one import
## descriptor, as read-only ones do.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: lld-link -def:a.def -out:a.lib -machine:x64
# RUN: lld-link -def:b.def -out:b.lib -machine:x64
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc p1.s -o p1.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc p2.s -o p2.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc p3.s -o p3.obj
# RUN: lld-link -import-slots -entry:main -subsystem:console main.obj p1.obj \
# RUN:   p2.obj p3.obj a.lib b.lib -out:main.exe
# RUN: llvm-readobj --sections --coff-imports main.exe | FileCheck %s

# CHECK:      Name: .data
# CHECK-NEXT: VirtualSize: 0x20
# CHECK-NEXT: VirtualAddress: 0x[[#%X,DATA:]]
# CHECK:      Name: a.dll
# CHECK-NEXT: ImportLookupTableRVA:
# CHECK-NEXT: ImportAddressTableRVA: 0x[[#%X,DATA+8]]
# CHECK-NEXT: Symbol: f1 (0)
# CHECK-NEXT: Symbol: f2 (1)
# CHECK-NEXT: }
# CHECK-NEXT: Import {
# CHECK-NEXT: Name: b.dll
# CHECK-NEXT: ImportLookupTableRVA:
# CHECK-NEXT: ImportAddressTableRVA: 0x[[#%X,DATA+24]]
# CHECK-NEXT: Symbol: g (0)
# CHECK-NEXT: }
# CHECK-NOT:  Import {

#--- a.def
LIBRARY a.dll
EXPORTS
  f1
  f2

#--- b.def
LIBRARY b.dll
EXPORTS
  g

#--- main.s
  .text
  .globl main
main:
  retq

  .data
  .p2align 3
  .quad 1

#--- p1.s
  .data
  .p2align 3
  .quad f1

#--- p2.s
  .data
  .p2align 3
  .quad g

#--- p3.s
  .data
  .p2align 3
  .quad f2
